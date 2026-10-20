// Real video through the whole receive chain: frames encoded by ffmpeg are packetized, handed
// to the receiver packet by packet, reassembled, and decoded. Which codecs run depends on the
// encoders the linked ffmpeg was built with; a codec with no encoder is skipped.

#include "Core/Media/CodecSupport.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "VideoRtpSupport.hpp"

#include <QElapsedTimer>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

using namespace Acheron;
using namespace Acheron::Discord::Voice;
using namespace VideoRtpSupport;

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr int FRAME_COUNT = 30;
constexpr int KEYFRAME_INTERVAL = 10;
constexpr uint32_t SSRC = 77;

struct CodecCase
{
    VideoCodec codec;
    AVCodecID id;
    // Tried in turn: not every ffmpeg has the same ones.
    std::array<const char *, 2> encoders;
};

struct EncodedFrame
{
    Bytes data;
    bool keyframe = false;
    uint32_t timestamp = 0;
};

// A short clip of moving gradients, encoded with as little delay as the encoder allows.
std::vector<EncodedFrame> encodeClip(const CodecCase &codecCase)
{
    std::vector<EncodedFrame> frames;

    const AVCodec *encoder = nullptr;
    for (const char *name : codecCase.encoders) {
        if (!encoder && name)
            encoder = avcodec_find_encoder_by_name(name);
    }
    if (!encoder)
        return frames;

    AVCodecContext *context = avcodec_alloc_context3(encoder);
    context->width = WIDTH;
    context->height = HEIGHT;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = { 1, 30 };
    context->framerate = { 30, 1 };
    context->bit_rate = 400000;
    context->gop_size = KEYFRAME_INTERVAL;
    context->max_b_frames = 0;
    av_opt_set(context, "deadline", "realtime", AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context, "lag-in-frames", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context, "auto-alt-ref", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context, "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context, "usage", "realtime", AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context, "cpu-used", 8, AV_OPT_SEARCH_CHILDREN);
    // Kvazaar reorders frames unless told not to, which no live sender does.
    av_opt_set(context, "kvazaar-params", "gop=0,period=10", AV_OPT_SEARCH_CHILDREN);

    if (avcodec_open2(context, encoder, nullptr) < 0) {
        avcodec_free_context(&context);
        return frames;
    }

    AVFrame *picture = av_frame_alloc();
    picture->format = context->pix_fmt;
    picture->width = WIDTH;
    picture->height = HEIGHT;
    av_frame_get_buffer(picture, 0);
    AVPacket *packet = av_packet_alloc();

    const auto drain = [&] {
        while (avcodec_receive_packet(context, packet) >= 0) {
            EncodedFrame frame;
            frame.data.assign(packet->data, packet->data + packet->size);
            frame.keyframe = packet->flags & AV_PKT_FLAG_KEY;
            frame.timestamp = static_cast<uint32_t>(packet->pts * 3000);
            frames.push_back(std::move(frame));
            av_packet_unref(packet);
        }
    };

    for (int index = 0; index < FRAME_COUNT; index++) {
        av_frame_make_writable(picture);
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++)
                picture->data[0][y * picture->linesize[0] + x] = static_cast<uint8_t>(x + y + index * 3);
        }
        for (int y = 0; y < HEIGHT / 2; y++) {
            for (int x = 0; x < WIDTH / 2; x++) {
                picture->data[1][y * picture->linesize[1] + x] = static_cast<uint8_t>(128 + y + index * 2);
                picture->data[2][y * picture->linesize[2] + x] = static_cast<uint8_t>(64 + x + index * 5);
            }
        }
        picture->pts = index;
        if (avcodec_send_frame(context, picture) < 0)
            break;
        drain();
    }
    avcodec_send_frame(context, nullptr);
    drain();

    av_packet_free(&packet);
    av_frame_free(&picture);
    avcodec_free_context(&context);
    return frames;
}

class CountingSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *frame) override
    {
        width = frame->width;
        height = frame->height;
        frames++;
    }

    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
};

// Holds the decoder's thread in present() until the gate is opened.
class GatedSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *) override
    {
        frames++;
        std::unique_lock lock(mutex);
        opened.wait(lock, [this] { return open; });
    }

    void openGate()
    {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        opened.notify_all();
    }

    std::atomic<int> frames{ 0 };

private:
    std::mutex mutex;
    std::condition_variable opened;
    bool open = false;
};

EncodedVideoFrame forDecoder(VideoCodec codec, const EncodedFrame &frame)
{
    EncodedVideoFrame encoded;
    encoded.data = frame.data;
    encoded.rtpTimestamp = frame.timestamp;
    encoded.keyframe = frame.keyframe;
    prepareForDecoder(codec, encoded.data);
    return encoded;
}

struct Chain
{
    explicit Chain(VideoCodec codec, bool allowHardware = false)
        : codec(codec), receiver(SSRC, codec), sink(std::make_shared<CountingSink>()), decoder(allowHardware)
    {
        decoder.setSink(sink);
        decoder.setKeyframeRequester([this] { keyframeRequests++; });
    }

    // Sends one encoded frame as RTP, leaving out the packets whose index `dropped` accepts.
    void send(const CodecCase &codecCase, const EncodedFrame &frame, const std::function<bool(size_t)> &dropped = {})
    {
        const RtpPayloads packets = codec == VideoCodec::AV1 ? packetizeAv1ByHand(frame.data, frame.keyframe) : packetizeWithFfmpeg(codecCase.id, frame.data, frame.keyframe);
        for (size_t i = 0; i < packets.payloads.size(); i++) {
            const uint16_t number = sequence++;
            if (dropped && dropped(i))
                continue;
            PacketPool::Handle packet = pool.acquire();
            packet->append(packets.payloads[i]);
            receiver.insert(number, frame.timestamp, packets.markers[i], std::move(packet), now);
        }

        for (EncodedVideoFrame &assembled : receiver.takeFrames()) {
            prepareForDecoder(codec, assembled.data);
            decoder.push(codec, std::move(assembled));
            pushed++;
        }
    }

    // Waits for the decoder to show `count` frames and returns how many it did show.
    int waitForDecoded(int count)
    {
        QElapsedTimer timer;
        timer.start();
        while (sink->frames < count && timer.elapsed() < 5000)
            QThread::msleep(5);
        // Whatever is still in the decoder's queue gets its chance to fail.
        QThread::msleep(50);
        return sink->frames;
    }

    VideoCodec codec;
    PacketPool pool;
    VideoReceiver receiver;
    std::shared_ptr<CountingSink> sink;
    Core::Media::VideoDecoder decoder;
    std::atomic<int> keyframeRequests{ 0 };
    uint16_t sequence = 65500;
    qint64 now = 0;
    int pushed = 0;
};

} // namespace

Q_DECLARE_METATYPE(CodecCase)

class TestVideoDecode : public QObject
{
    Q_OBJECT
private slots:
    void decodableCodecsAreOffered();

    void everyFrameDecodes_data() { codecCases(); }
    void everyFrameDecodes();
    void lossSkipsToTheNextKeyframe_data() { codecCases(); }
    void lossSkipsToTheNextKeyframe();
    void hardwareDecodesOrGivesWayToSoftware_data() { codecCases(); }
    void hardwareDecodesOrGivesWayToSoftware();
    void aDiscardedQueueStartsOverAtAKeyframe();
    void aQueueThatHasWaitedTooLongIsDiscarded();
    void aDamagedStreamIsNotHeldAgainstTheHardware();

private:
    static void codecCases();
};

void TestVideoDecode::codecCases()
{
    QTest::addColumn<CodecCase>("codecCase");
    QTest::newRow("H264") << CodecCase{ VideoCodec::H264, AV_CODEC_ID_H264, { "libopenh264", "libx264" } };
    QTest::newRow("H265") << CodecCase{ VideoCodec::H265, AV_CODEC_ID_HEVC, { "libkvazaar", "libx265" } };
    QTest::newRow("VP8") << CodecCase{ VideoCodec::VP8, AV_CODEC_ID_VP8, { "libvpx" } };
    QTest::newRow("AV1") << CodecCase{ VideoCodec::AV1, AV_CODEC_ID_AV1, { "libaom-av1" } };
}

void TestVideoDecode::decodableCodecsAreOffered()
{
    const QList<VideoCodec> decodable = Core::Media::decodableVideoCodecs();
    const QList<Codec> offered = Core::Media::offeredVideoCodecs();
    QVERIFY(decodable.contains(VideoCodec::H264));
    QCOMPARE(offered.size(), decodable.size());

    // Payload types follow the position in the list, as the official client assigns them.
    for (int i = 0; i < offered.size(); i++) {
        QCOMPARE(offered[i].name.get(), videoCodecName(decodable[i]));
        QCOMPARE(offered[i].payloadType.get(), 101 + 2 * i);
        QCOMPARE(offered[i].rtxPayloadType.get(), 102 + 2 * i);
        QCOMPARE(offered[i].priority.get(), 1000 * (i + 1));
        QCOMPARE(offered[i].decode.get(), true);
        QCOMPARE(offered[i].encode.get(), false);
    }

    // The preference order is AV1, H265, H264, VP8, whichever of them are present.
    QList<VideoCodec> expected;
    for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 }) {
        if (decodable.contains(codec))
            expected.append(codec);
    }
    QVERIFY(decodable == expected);
}

void TestVideoDecode::everyFrameDecodes()
{
    QFETCH(CodecCase, codecCase);
    if (!Core::Media::decodableVideoCodecs().contains(codecCase.codec))
        QSKIP("this ffmpeg cannot decode the codec");
    const std::vector<EncodedFrame> clip = encodeClip(codecCase);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");
    QCOMPARE(clip.size(), size_t(FRAME_COUNT));

    Chain chain(codecCase.codec);
    for (const EncodedFrame &frame : clip)
        chain.send(codecCase, frame);

    QCOMPARE(chain.pushed, FRAME_COUNT);
    const int shown = chain.waitForDecoded(FRAME_COUNT);
    QCOMPARE(chain.decoder.stats().errors, quint64(0));
    QCOMPARE(chain.decoder.stats().dropped, quint64(0));
    QCOMPARE(chain.keyframeRequests.load(), 0);
    QCOMPARE(shown, FRAME_COUNT);
    QCOMPARE(chain.sink->width.load(), WIDTH);
    QCOMPARE(chain.sink->height.load(), HEIGHT);
}

void TestVideoDecode::lossSkipsToTheNextKeyframe()
{
    QFETCH(CodecCase, codecCase);
    if (!Core::Media::decodableVideoCodecs().contains(codecCase.codec))
        QSKIP("this ffmpeg cannot decode the codec");
    const std::vector<EncodedFrame> clip = encodeClip(codecCase);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");

    // The first packet of the fourth frame never arrives, and no retransmission comes.
    constexpr int LOST_FRAME = 3;
    Chain chain(codecCase.codec);
    int expected = 0;
    bool waitingForKeyframe = false;
    for (int index = 0; index < static_cast<int>(clip.size()); index++) {
        if (index == LOST_FRAME) {
            chain.send(codecCase, clip[index], [](size_t packet) { return packet == 0; });
            waitingForKeyframe = true;
            continue;
        }
        if (index == LOST_FRAME + 2) {
            // Long enough for the receiver to stop asking for the packet.
            chain.now += VideoReceiver::NACK_GIVE_UP_MS + 1;
            QVERIFY(chain.receiver.poll(chain.now).keyframeRequest);
        }

        if (clip[index].keyframe && index > LOST_FRAME + 2)
            waitingForKeyframe = false;
        if (!waitingForKeyframe)
            expected++;
        chain.send(codecCase, clip[index]);
    }

    // Everything from the loss up to the next key frame is withheld, so nothing the decoder
    // was given depended on the missing frame.
    QVERIFY(expected < static_cast<int>(clip.size()));
    QCOMPARE(chain.pushed, expected);
    const int shown = chain.waitForDecoded(expected);
    QCOMPARE(chain.decoder.stats().errors, quint64(0));
    QCOMPARE(chain.keyframeRequests.load(), 0);
    QCOMPARE(shown, expected);
}

void TestVideoDecode::hardwareDecodesOrGivesWayToSoftware()
{
    QFETCH(CodecCase, codecCase);
    if (!Core::Media::decodableVideoCodecs().contains(codecCase.codec))
        QSKIP("this ffmpeg cannot decode the codec");
    // Twice over, so that a stream the graphics card turns down is seen to restart in software.
    std::vector<EncodedFrame> clip = encodeClip(codecCase);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");
    const size_t once = clip.size();
    for (size_t i = 0; i < once; i++) {
        EncodedFrame again = clip[i];
        again.timestamp += static_cast<uint32_t>(once) * 3000;
        clip.push_back(std::move(again));
    }

    Chain chain(codecCase.codec, true);
    for (const EncodedFrame &frame : clip)
        chain.send(codecCase, frame);
    chain.waitForDecoded(static_cast<int>(clip.size()));

    const Core::Media::VideoDecoder::Stats stats = chain.decoder.stats();
    qInfo() << "decoder:" << stats.decoderName << "decoded" << stats.decoded << "errors" << stats.errors;
    QVERIFY(!stats.decoderName.isEmpty());
    QCOMPARE(chain.sink->width.load(), WIDTH);
    QCOMPARE(chain.sink->height.load(), HEIGHT);
    if (stats.hardware) {
        // The graphics card took it: nothing may have gone wrong on the way.
        QCOMPARE(stats.errors, quint64(0));
        QCOMPARE(chain.sink->frames.load(), static_cast<int>(clip.size()));
    } else {
        // Software, either from the start or after the hardware gave up early on.
        QVERIFY(stats.errors <= 1);
        QVERIFY(chain.sink->frames.load() >= FRAME_COUNT);
    }
}

void TestVideoDecode::aDiscardedQueueStartsOverAtAKeyframe()
{
    const CodecCase vp8{ VideoCodec::VP8, AV_CODEC_ID_VP8, { "libvpx" } };
    if (!Core::Media::decodableVideoCodecs().contains(vp8.codec))
        QSKIP("this ffmpeg cannot decode the codec");
    const std::vector<EncodedFrame> clip = encodeClip(vp8);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");
    QVERIFY(clip[0].keyframe && !clip[1].keyframe && clip[KEYFRAME_INTERVAL].keyframe);

    auto sink = std::make_shared<GatedSink>();
    std::atomic<int> keyframeRequests{ 0 };
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);
    decoder.setKeyframeRequester([&keyframeRequests] { keyframeRequests++; });

    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[0]));
    QTRY_COMPARE(sink->frames.load(), 1);

    // With its thread held in the sink the decoder falls behind by more than its queue holds,
    // so the queue is thrown away and what arrives next has nothing to build on.
    constexpr int ARRIVING_AFTER_THE_DISCARD = 5;
    for (int i = 0; i < Core::Media::VideoDecoder::MAX_QUEUED_FRAMES + ARRIVING_AFTER_THE_DISCARD; i++)
        decoder.push(vp8.codec, forDecoder(vp8.codec, clip[1]));
    QCOMPARE(keyframeRequests.load(), 1);

    sink->openGate();
    QTest::qWait(300);
    QCOMPARE(sink->frames.load(), 1);
    QCOMPARE(decoder.stats().errors, quint64(0));

    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[KEYFRAME_INTERVAL]));
    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[KEYFRAME_INTERVAL + 1]));
    QTRY_COMPARE(sink->frames.load(), 3);
}

void TestVideoDecode::aQueueThatHasWaitedTooLongIsDiscarded()
{
    const CodecCase vp8{ VideoCodec::VP8, AV_CODEC_ID_VP8, { "libvpx" } };
    if (!Core::Media::decodableVideoCodecs().contains(vp8.codec))
        QSKIP("this ffmpeg cannot decode the codec");
    const std::vector<EncodedFrame> clip = encodeClip(vp8);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");

    auto sink = std::make_shared<GatedSink>();
    std::atomic<int> keyframeRequests{ 0 };
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);
    decoder.setKeyframeRequester([&keyframeRequests] { keyframeRequests++; });

    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[0]));
    QTRY_COMPARE(sink->frames.load(), 1);

    // A slow stream never fills the queue, but what waits in it is as far behind all the same.
    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[1]));
    QTest::qWait(Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2);
    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[1]));
    QCOMPARE(keyframeRequests.load(), 0);
    QVERIFY(decoder.stats().backlogMs >= Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2);
    QTest::qWait(Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2 + 100);
    decoder.push(vp8.codec, forDecoder(vp8.codec, clip[1]));
    QCOMPARE(keyframeRequests.load(), 1);
    QCOMPARE(decoder.stats().dropped, quint64(2));
    QCOMPARE(decoder.stats().queuesDiscarded, quint64(1));

    sink->openGate();
    QTest::qWait(200);
    QCOMPARE(sink->frames.load(), 1);
}

void TestVideoDecode::aDamagedStreamIsNotHeldAgainstTheHardware()
{
    const CodecCase h264{ VideoCodec::H264, AV_CODEC_ID_H264, { "libopenh264", "libx264" } };
    const std::vector<EncodedFrame> clip = encodeClip(h264);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");

    struct Outcome
    {
        bool hardware = false;
        quint64 errors = 0;
        int shown = 0;
    };
    const auto decodeClip = [&](int damagedKeyframesFirst) {
        Chain chain(h264.codec, true);
        for (int i = 0; i < damagedKeyframesFirst; i++) {
            // An IDR slice with nothing to say what picture it belongs to.
            EncodedVideoFrame damaged;
            damaged.data = { 0, 0, 0, 1, 0x65, 0xB8, 0x00, 0x04, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            damaged.keyframe = true;
            chain.decoder.push(h264.codec, std::move(damaged));
        }
        for (const EncodedFrame &frame : clip)
            chain.send(h264, frame);
        Outcome outcome;
        outcome.shown = chain.waitForDecoded(static_cast<int>(clip.size()));
        outcome.hardware = chain.decoder.stats().hardware;
        outcome.errors = chain.decoder.stats().errors;
        return outcome;
    };

    if (!decodeClip(0).hardware)
        QSKIP("no hardware H264 decoder on this machine");

    // More damaged starts than failures are tolerated before hardware is given up on.
    for (int attempt = 0; attempt < 3; attempt++) {
        const Outcome outcome = decodeClip(1);
        QCOMPARE(outcome.errors, quint64(1));
        QVERIFY(outcome.hardware);
        QCOMPARE(outcome.shown, static_cast<int>(clip.size()));
    }
    QVERIFY(decodeClip(0).hardware);
}

QTEST_GUILESS_MAIN(TestVideoDecode)
#include "tst_VideoDecode.moc"
