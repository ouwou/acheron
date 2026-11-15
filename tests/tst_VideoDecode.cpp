#include "Core/Media/CodecSupport.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "VideoRtpSupport.hpp"

#include <QCoreApplication>
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

using Hardware = Core::Media::VideoDecoder::Hardware;

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr int FRAMERATE = 30;
constexpr uint32_t TICKS_PER_FRAME = 90000 / FRAMERATE;
constexpr int FRAME_COUNT = 30;
constexpr int KEYFRAME_INTERVAL = 10;
constexpr uint32_t SSRC = 77;
const char NO_HARDWARE_H264[] = "no hardware H264 decoder on this machine";

struct CodecCase
{
    VideoCodec codec;
    AVCodecID id;
    std::array<const char *, 2> encoders;
};

constexpr CodecCase H264_CASE{ VideoCodec::H264, AV_CODEC_ID_H264, { "libopenh264", "libx264" } };
constexpr CodecCase H265_CASE{ VideoCodec::H265, AV_CODEC_ID_HEVC, { "libkvazaar", "libx265" } };
constexpr CodecCase VP8_CASE{ VideoCodec::VP8, AV_CODEC_ID_VP8, { "libvpx" } };
constexpr CodecCase AV1_CASE{ VideoCodec::AV1, AV_CODEC_ID_AV1, { "libaom-av1" } };

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
    const Core::Media::CodecContextPointer context(encoder ? avcodec_alloc_context3(encoder) : nullptr);
    if (!context)
        return frames;
    context->width = WIDTH;
    context->height = HEIGHT;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = { 1, FRAMERATE };
    context->framerate = { FRAMERATE, 1 };
    context->bit_rate = 400000;
    context->gop_size = KEYFRAME_INTERVAL;
    context->max_b_frames = 0;
    av_opt_set(context.get(), "deadline", "realtime", AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context.get(), "lag-in-frames", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context.get(), "auto-alt-ref", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context.get(), "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context.get(), "usage", "realtime", AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(context.get(), "cpu-used", 8, AV_OPT_SEARCH_CHILDREN);
    // kvazaar reorders frames unless told not to, which no live sender does
    av_opt_set(context.get(), "kvazaar-params", "gop=0,period=10", AV_OPT_SEARCH_CHILDREN);
    if (avcodec_open2(context.get(), encoder, nullptr) < 0)
        return frames;

    const Core::Media::FramePointer picture(av_frame_alloc());
    const Core::Media::PacketPointer packet(av_packet_alloc());
    const auto drain = [&] {
        while (avcodec_receive_packet(context.get(), packet.get()) >= 0) {
            EncodedFrame frame;
            frame.data.assign(packet->data, packet->data + packet->size);
            frame.keyframe = packet->flags & AV_PKT_FLAG_KEY;
            frame.timestamp = static_cast<uint32_t>(packet->pts * TICKS_PER_FRAME);
            frames.push_back(std::move(frame));
            av_packet_unref(packet.get());
        }
    };

    for (int index = 0; index < FRAME_COUNT; index++) {
        if (Core::Media::prepareWritable(picture.get(), AV_PIX_FMT_YUV420P, QSize(WIDTH, HEIGHT)) < 0)
            break;
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
        if (avcodec_send_frame(context.get(), picture.get()) < 0)
            break;
        drain();
    }
    avcodec_send_frame(context.get(), nullptr);
    drain();
    return frames;
}

class CountingSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *frame) override
    {
        width = frame->width;
        height = frame->height;
        quarterTurns = Core::Media::quarterTurnsOf(frame);
        frames++;
    }

    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
    std::atomic<int> quarterTurns{ -1 };
};

class SlowSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *) override
    {
        QThread::msleep(30);
        frames++;
    }

    std::atomic<int> frames{ 0 };
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
    explicit Chain(const CodecCase &codecCase, Hardware hardware = Hardware::Off)
        : codecCase(codecCase), receiver(SSRC, codecCase.codec), sink(std::make_shared<CountingSink>()), decoder(hardware)
    {
        decoder.setSink(sink);
        decoder.setKeyframeRequester([this] { keyframeRequests++; });
    }

    void send(const EncodedFrame &frame, const std::function<bool(size_t)> &dropped = {})
    {
        const VideoCodec codec = codecCase.codec;
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
            assembled.quarterTurns = quarterTurns;
            decoder.push(codec, std::move(assembled));
            pushed++;
        }
    }

    int waitForDecoded(int count)
    {
        if (QTest::qWaitFor([&] { return sink->frames >= count; }, 5000))
            QThread::msleep(50);
        return sink->frames;
    }

    const CodecCase codecCase;
    PacketPool pool;
    VideoReceiver receiver;
    std::shared_ptr<CountingSink> sink;
    std::atomic<int> keyframeRequests{ 0 };
    Core::Media::VideoDecoder decoder;
    uint16_t sequence = 65500;
    qint64 now = 0;
    int pushed = 0;
    int quarterTurns = 0;
};

struct ClipOutcome
{
    bool hardware = false;
    quint64 errors = 0;
    int shown = 0;
};

ClipOutcome decodeH264Clip(const std::vector<EncodedFrame> &clip, int damagedKeyframesFirst = 0)
{
    Chain chain(H264_CASE, Hardware::Allowed);
    for (int i = 0; i < damagedKeyframesFirst; i++) {
        EncodedVideoFrame damaged;
        damaged.data = { 0, 0, 0, 1, 0x65, 0xB8, 0x00, 0x04, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        damaged.keyframe = true;
        chain.decoder.push(VideoCodec::H264, std::move(damaged));
    }
    for (const EncodedFrame &frame : clip)
        chain.send(frame);
    ClipOutcome outcome;
    outcome.shown = chain.waitForDecoded(static_cast<int>(clip.size()));
    outcome.hardware = chain.decoder.stats().hardware;
    outcome.errors = chain.decoder.stats().errors;
    return outcome;
}

struct GatedDecoder
{
    GatedDecoder() : sink(std::make_shared<GatedSink>()), decoder(Hardware::Off)
    {
        decoder.setSink(sink);
        decoder.setKeyframeRequester([this] { keyframeRequests++; });
    }

    // a test that ends before opening the gate would otherwise leave the decoder's join waiting forever
    ~GatedDecoder() { sink->openGate(); }

    void push(const EncodedFrame &frame) { decoder.push(VideoCodec::VP8, forDecoder(VideoCodec::VP8, frame)); }

    std::shared_ptr<GatedSink> sink;
    std::atomic<int> keyframeRequests{ 0 };
    Core::Media::VideoDecoder decoder;
};

} // namespace

Q_DECLARE_METATYPE(CodecCase)

#define ENCODE_CLIP_OR_SKIP(clip, codecCase)                              \
    if (!Core::Media::decodableVideoCodecs().contains((codecCase).codec)) \
        QSKIP("this ffmpeg cannot decode the codec");                     \
    const std::vector<EncodedFrame> clip = encodeClip(codecCase);         \
    if (clip.empty())                                                     \
    QSKIP("this ffmpeg has no encoder for the codec")

class TestVideoDecode : public QObject
{
    Q_OBJECT
private slots:
    void decodableCodecsAreOffered();
    void aCodecTurnedOffIsNoLongerOfferedForDecoding();
    void aPictureSaysHowItIsToBeTurned_data();
    void aPictureSaysHowItIsToBeTurned();

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
    // last, as a codec given up stays given up
    void aCodecLeavingNoTimeToSpareIsGivenUpDownToH264();

private:
    static void codecCases();
};

void TestVideoDecode::codecCases()
{
    QTest::addColumn<CodecCase>("codecCase");
    QTest::newRow("H264") << H264_CASE;
    QTest::newRow("H265") << H265_CASE;
    QTest::newRow("VP8") << VP8_CASE;
    QTest::newRow("AV1") << AV1_CASE;
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

void TestVideoDecode::aCodecTurnedOffIsNoLongerOfferedForDecoding()
{
    QCoreApplication::setOrganizationName(QStringLiteral("AcheronTests"));
    const QList<Codec> before = Core::Media::offeredVideoCodecs();
    QVERIFY(!before.isEmpty());
    const VideoCodec turnedOff = Core::Media::decodableVideoCodecs().first();

    Core::Media::setReceiveCodecAllowed(turnedOff, false);
    const bool stillAllowed = Core::Media::receiveCodecAllowed(turnedOff);
    const QList<Codec> after = Core::Media::offeredVideoCodecs({ turnedOff });
    Core::Media::setReceiveCodecAllowed(turnedOff, true);

    QVERIFY(!stillAllowed);
    QVERIFY(Core::Media::receiveCodecAllowed(turnedOff));
    QCOMPARE(after.size(), before.size());
    for (int i = 0; i < after.size(); i++) {
        QCOMPARE(after[i].payloadType.get(), before[i].payloadType.get());
        QCOMPARE(after[i].decode.get(), i != 0);
    }
    QCOMPARE(after[0].encode.get(), true);
}

void TestVideoDecode::aPictureSaysHowItIsToBeTurned_data()
{
    QTest::addColumn<int>("quarterTurns");
    for (int turns = 0; turns < 4; turns++)
        QTest::addRow("%d quarter turns", turns) << turns;
}

void TestVideoDecode::aPictureSaysHowItIsToBeTurned()
{
    QFETCH(int, quarterTurns);
    ENCODE_CLIP_OR_SKIP(clip, H264_CASE);

    Chain chain(H264_CASE);
    chain.quarterTurns = quarterTurns;
    for (const EncodedFrame &frame : clip)
        chain.send(frame);
    QCOMPARE(chain.waitForDecoded(static_cast<int>(clip.size())), static_cast<int>(clip.size()));
    QCOMPARE(chain.sink->quarterTurns.load(), quarterTurns);
}

void TestVideoDecode::everyFrameDecodes()
{
    QFETCH(CodecCase, codecCase);
    ENCODE_CLIP_OR_SKIP(clip, codecCase);
    QCOMPARE(clip.size(), size_t(FRAME_COUNT));

    Chain chain(codecCase);
    for (const EncodedFrame &frame : clip)
        chain.send(frame);

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
    ENCODE_CLIP_OR_SKIP(clip, codecCase);

    constexpr int LOST_FRAME = 3;
    Chain chain(codecCase);
    int expected = 0;
    bool waitingForKeyframe = false;
    for (int index = 0; index < static_cast<int>(clip.size()); index++) {
        if (index == LOST_FRAME) {
            chain.send(clip[index], [](size_t packet) { return packet == 0; });
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
        chain.send(clip[index]);
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
    ENCODE_CLIP_OR_SKIP(clip, codecCase);
    std::vector<EncodedFrame> twice = clip;
    for (EncodedFrame again : clip) {
        again.timestamp += static_cast<uint32_t>(clip.size()) * TICKS_PER_FRAME;
        twice.push_back(std::move(again));
    }

    Chain chain(codecCase, Hardware::Allowed);
    for (const EncodedFrame &frame : twice)
        chain.send(frame);
    chain.waitForDecoded(static_cast<int>(twice.size()));

    const Core::Media::VideoDecoder::Stats stats = chain.decoder.stats();
    qInfo() << "decoder:" << stats.decoderName << "decoded" << stats.decoded << "errors" << stats.errors;
    QVERIFY(!stats.decoderName.isEmpty());
    QCOMPARE(chain.sink->width.load(), WIDTH);
    QCOMPARE(chain.sink->height.load(), HEIGHT);
    if (stats.hardware) {
        QCOMPARE(stats.errors, quint64(0));
        QCOMPARE(chain.sink->frames.load(), static_cast<int>(twice.size()));
    } else {
        QVERIFY(stats.errors <= 1);
        QVERIFY(chain.sink->frames.load() >= FRAME_COUNT);
    }
}

void TestVideoDecode::aDiscardedQueueStartsOverAtAKeyframe()
{
    ENCODE_CLIP_OR_SKIP(clip, VP8_CASE);
    QVERIFY(clip[0].keyframe && !clip[1].keyframe && clip[KEYFRAME_INTERVAL].keyframe);

    GatedDecoder gated;
    gated.push(clip[0]);
    QTRY_COMPARE(gated.sink->frames.load(), 1);

    constexpr int ARRIVING_AFTER_THE_DISCARD = 5;
    for (int i = 0; i < Core::Media::VideoDecoder::MAX_QUEUED_FRAMES + ARRIVING_AFTER_THE_DISCARD; i++)
        gated.push(clip[1]);
    QCOMPARE(gated.keyframeRequests.load(), 1);

    gated.sink->openGate();
    QTest::qWait(300);
    QCOMPARE(gated.sink->frames.load(), 1);
    QCOMPARE(gated.decoder.stats().errors, quint64(0));

    gated.push(clip[KEYFRAME_INTERVAL]);
    gated.push(clip[KEYFRAME_INTERVAL + 1]);
    QTRY_COMPARE(gated.sink->frames.load(), 3);
}

void TestVideoDecode::aQueueThatHasWaitedTooLongIsDiscarded()
{
    ENCODE_CLIP_OR_SKIP(clip, VP8_CASE);

    GatedDecoder gated;
    gated.push(clip[0]);
    QTRY_COMPARE(gated.sink->frames.load(), 1);

    gated.push(clip[1]);
    QTest::qWait(Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2);
    gated.push(clip[1]);
    QCOMPARE(gated.keyframeRequests.load(), 0);
    QVERIFY(gated.decoder.stats().backlogMs >= Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2);
    QTest::qWait(Core::Media::VideoDecoder::MAX_QUEUE_AGE_MS / 2 + 100);
    gated.push(clip[1]);
    QCOMPARE(gated.keyframeRequests.load(), 1);
    QCOMPARE(gated.decoder.stats().dropped, quint64(2));
    QCOMPARE(gated.decoder.stats().queuesDiscarded, quint64(1));

    gated.sink->openGate();
    QTest::qWait(200);
    QCOMPARE(gated.sink->frames.load(), 1);
}

void TestVideoDecode::aDamagedStreamIsNotHeldAgainstTheHardware()
{
    ENCODE_CLIP_OR_SKIP(clip, H264_CASE);
    if (!decodeH264Clip(clip).hardware)
        QSKIP(NO_HARDWARE_H264);

    for (int attempt = 0; attempt < 3; attempt++) {
        const ClipOutcome outcome = decodeH264Clip(clip, 1);
        QCOMPARE(outcome.errors, quint64(1));
        QVERIFY(outcome.hardware);
        QCOMPARE(outcome.shown, static_cast<int>(clip.size()));
    }
    QVERIFY(decodeH264Clip(clip).hardware);
}

void TestVideoDecode::plainBaselineDecodesOnTheCard()
{
    ENCODE_CLIP_OR_SKIP(clip, H264_CASE);
    if (!decodeH264Clip(clip).hardware)
        QSKIP(NO_HARDWARE_H264);

    const ClipOutcome baseline = decodeH264Clip(withH264Profile(clip, H264_PROFILE_BASELINE));
    QVERIFY(baseline.hardware);
    QCOMPARE(baseline.shown, static_cast<int>(clip.size()));
}

void TestVideoDecode::aStreamTheCardRefusesIsNotHeldAgainstTheCodec()
{
    ENCODE_CLIP_OR_SKIP(clip, H264_CASE);
    if (!decodeH264Clip(clip).hardware)
        QSKIP(NO_HARDWARE_H264);

    const std::vector<EncodedFrame> refused = withH264Profile(clip, H264_PROFILE_EXTENDED);
    if (decodeH264Clip(refused).hardware)
        QSKIP("the card decodes extended profile streams too");
    for (int attempt = 0; attempt < 3; attempt++) {
        const ClipOutcome outcome = decodeH264Clip(refused);
        QVERIFY(!outcome.hardware);
        QCOMPARE(outcome.shown, static_cast<int>(clip.size()));
    }
    QVERIFY(decodeH264Clip(clip).hardware);
}

void TestVideoDecode::aCodecLeavingNoTimeToSpareIsGivenUpDownToH264()
{
    struct Strained
    {
        explicit Strained(const CodecCase &codecCase) : codec(codecCase.codec), clip(encodeClip(codecCase)), sink(std::make_shared<SlowSink>()), decoder(Hardware::Off)
        {
            decoder.setSink(sink);
            decoder.setCodecFailureHandler([this](VideoCodec) { reports++; });
        }

        // the decoder never finds its queue empty, which is what strain is
        void keepBusy()
        {
            if (pushed - static_cast<size_t>(sink->frames.load()) > 1)
                return;
            decoder.push(codec, forDecoder(codec, clip[pushed % clip.size()]));
            pushed++;
        }

        VideoCodec codec;
        std::vector<EncodedFrame> clip;
        std::shared_ptr<SlowSink> sink;
        std::atomic<int> reports{ 0 };
        Core::Media::VideoDecoder decoder;
        size_t pushed = 0;
    };

    Strained heavy(H265_CASE);
    Strained light(H264_CASE);
    if (heavy.clip.empty() || light.clip.empty())
        QSKIP("this ffmpeg has no encoder for the codec");

    QElapsedTimer timer;
    timer.start();
    const qint64 longest = 2 * Core::Media::DecodeStrain::WINDOWS_IN_A_ROW * Core::Media::DecodeStrain::WINDOW_MS;
    while (heavy.reports == 0 && timer.elapsed() < longest) {
        heavy.keepBusy();
        light.keepBusy();
        QThread::msleep(1);
    }

    QCOMPARE(heavy.reports.load(), 1);
    QCOMPARE(light.reports.load(), 0);
    QVERIFY(Core::Media::decodingGivenUpOn(VideoCodec::H265));
    QVERIFY(!Core::Media::decodingGivenUpOn(VideoCodec::H264));

    for (const Codec &offered : Core::Media::offeredVideoCodecs())
        QCOMPARE(offered.decode.get(), videoCodecFromName(offered.name.get()) != VideoCodec::H265);
}

QTEST_GUILESS_MAIN(TestVideoDecode)
#include "tst_VideoDecode.moc"
