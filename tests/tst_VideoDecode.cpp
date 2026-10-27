
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
    std::array<const char *, 2> encoders;
};

struct EncodedFrame
{
    Bytes data;
    bool keyframe = false;
    uint32_t timestamp = 0;
};

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
    // kvazaar reorders frames unless told not to, which no live sender does
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

constexpr uint8_t H264_PROFILE_BASELINE = 66;
constexpr uint8_t H264_PROFILE_EXTENDED = 88;
constexpr uint8_t H264_CONSTRAINT_SET1 = 0x40;
constexpr uint8_t H264_NAL_SPS = 7;

// baseline and extended SPSs share their syntax, so only profile_idc and the constraint flags change
std::vector<EncodedFrame> withH264Profile(std::vector<EncodedFrame> clip, uint8_t profile)
{
    for (EncodedFrame &frame : clip) {
        Bytes &data = frame.data;
        for (size_t i = 0; i + 5 < data.size(); i++) {
            if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1 || (data[i + 3] & 0x1F) != H264_NAL_SPS)
                continue;
            data[i + 4] = profile;
            data[i + 5] &= static_cast<uint8_t>(~H264_CONSTRAINT_SET1);
        }
    }
    return clip;
}

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

    int waitForDecoded(int count)
    {
        QElapsedTimer timer;
        timer.start();
        while (sink->frames < count && timer.elapsed() < 5000)
            QThread::msleep(5);
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
    void plainBaselineDecodesOnTheCard();
    void aStreamTheCardRefusesIsNotHeldAgainstTheCodec();

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

    // payload types follow the position in the list, as the official client assigns them
    for (int i = 0; i < offered.size(); i++) {
        QCOMPARE(offered[i].name.get(), videoCodecName(decodable[i]));
        QCOMPARE(offered[i].payloadType.get(), 101 + 2 * i);
        QCOMPARE(offered[i].rtxPayloadType.get(), 102 + 2 * i);
        QCOMPARE(offered[i].priority.get(), 1000 * (i + 1));
        QCOMPARE(offered[i].decode.get(), true);
        QCOMPARE(offered[i].encode.get(), false);
    }

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
            chain.now += VideoReceiver::NACK_GIVE_UP_MS + 1;
            QVERIFY(chain.receiver.poll(chain.now).keyframeRequest);
        }

        if (clip[index].keyframe && index > LOST_FRAME + 2)
            waitingForKeyframe = false;
        if (!waitingForKeyframe)
            expected++;
        chain.send(codecCase, clip[index]);
    }

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
        QCOMPARE(stats.errors, quint64(0));
        QCOMPARE(chain.sink->frames.load(), static_cast<int>(clip.size()));
    } else {
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

    for (int attempt = 0; attempt < 3; attempt++) {
        const Outcome outcome = decodeClip(1);
        QCOMPARE(outcome.errors, quint64(1));
        QVERIFY(outcome.hardware);
        QCOMPARE(outcome.shown, static_cast<int>(clip.size()));
    }
    QVERIFY(decodeClip(0).hardware);
}

namespace {

struct ClipOutcome
{
    bool hardware = false;
    int shown = 0;
};

ClipOutcome decodeH264Clip(const CodecCase &h264, const std::vector<EncodedFrame> &clip)
{
    Chain chain(h264.codec, true);
    for (const EncodedFrame &frame : clip)
        chain.send(h264, frame);
    ClipOutcome outcome;
    outcome.shown = chain.waitForDecoded(static_cast<int>(clip.size()));
    outcome.hardware = chain.decoder.stats().hardware;
    return outcome;
}

} // namespace

void TestVideoDecode::plainBaselineDecodesOnTheCard()
{
    const CodecCase h264{ VideoCodec::H264, AV_CODEC_ID_H264, { "libopenh264", "libx264" } };
    const std::vector<EncodedFrame> clip = encodeClip(h264);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");
    if (!decodeH264Clip(h264, clip).hardware)
        QSKIP("no hardware H264 decoder on this machine");

    const ClipOutcome baseline = decodeH264Clip(h264, withH264Profile(clip, H264_PROFILE_BASELINE));
    QVERIFY(baseline.hardware);
    QCOMPARE(baseline.shown, static_cast<int>(clip.size()));
}

void TestVideoDecode::aStreamTheCardRefusesIsNotHeldAgainstTheCodec()
{
    const CodecCase h264{ VideoCodec::H264, AV_CODEC_ID_H264, { "libopenh264", "libx264" } };
    const std::vector<EncodedFrame> clip = encodeClip(h264);
    if (clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");
    if (!decodeH264Clip(h264, clip).hardware)
        QSKIP("no hardware H264 decoder on this machine");

    const std::vector<EncodedFrame> refused = withH264Profile(clip, H264_PROFILE_EXTENDED);
    if (decodeH264Clip(h264, refused).hardware)
        QSKIP("the card decodes extended profile streams too");
    for (int attempt = 0; attempt < 3; attempt++) {
        const ClipOutcome outcome = decodeH264Clip(h264, refused);
        QVERIFY(!outcome.hardware);
        QCOMPARE(outcome.shown, static_cast<int>(clip.size()));
    }
    QVERIFY(decodeH264Clip(h264, clip).hardware);
}

QTEST_GUILESS_MAIN(TestVideoDecode)
#include "tst_VideoDecode.moc"
