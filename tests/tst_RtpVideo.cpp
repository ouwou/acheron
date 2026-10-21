// packets for the depacketizers come from ffmpeg's RTP muxer, so the reference isnt this code's own
// idea of the formats. AV1 is also packetized by hand, since the muxer only learnt it recently

#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/TransportFeedback.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "VideoFrameHelpers.hpp"
#include "VideoRtpSupport.hpp"

#include <bytes/bytes.h>
#include <dave/dave_interfaces.h>

#include <QTest>

#include <random>

using namespace Acheron::Discord::Voice;

namespace {

using namespace VideoFrameHelpers;
using namespace VideoRtpSupport;
constexpr uint32_t SSRC = 0x1234;

void append(Bytes &out, const Bytes &bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendNal(Bytes &frame, std::initializer_list<uint8_t> header, size_t payloadSize, uint32_t seed, int firstByteOr = 0)
{
    frame.insert(frame.end(), { 0, 0, 0, 1 });
    frame.insert(frame.end(), header);
    Bytes payload = filler(payloadSize, seed);
    payload[0] |= static_cast<uint8_t>(firstByteOr);
    append(frame, payload);
}

Bytes h264Keyframe()
{
    Bytes frame;
    appendNal(frame, { 0x67 }, 20, 1);
    appendNal(frame, { 0x68 }, 6, 2);
    appendNal(frame, { 0x65 }, 5000, 3, 0x80);
    return frame;
}

Bytes h264DeltaFrame()
{
    Bytes frame;
    appendNal(frame, { 0x06 }, 12, 4);
    appendNal(frame, { 0x41 }, 3000, 5, 0x80);
    return frame;
}

Bytes h265Keyframe()
{
    Bytes frame;
    appendNal(frame, { 0x40, 0x01 }, 22, 6);
    appendNal(frame, { 0x42, 0x01 }, 30, 7);
    appendNal(frame, { 0x44, 0x01 }, 8, 8);
    appendNal(frame, { 0x26, 0x01 }, 5000, 9, 0x80);
    return frame;
}

Bytes h265DeltaFrame()
{
    Bytes frame;
    appendNal(frame, { 0x02, 0x01 }, 2500, 10, 0x80);
    return frame;
}

Bytes vp8Frame(bool keyframe, size_t size, uint32_t seed)
{
    Bytes frame = filler(size, seed);
    frame[0] = static_cast<uint8_t>(keyframe ? (frame[0] & ~1) : (frame[0] | 1));
    return frame;
}

void appendObu(Bytes &frame, uint8_t type, const Bytes &payload, bool withSize = true)
{
    frame.push_back(static_cast<uint8_t>((type << 3) | (withSize ? 0x02 : 0)));
    if (withSize)
        appendLeb128(frame, payload.size());
    append(frame, payload);
}

constexpr uint8_t OBU_SEQUENCE_HEADER = 1;
constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;
constexpr uint8_t OBU_FRAME = 6;

Bytes av1Frame(bool withDelimiter)
{
    Bytes frame;
    if (withDelimiter)
        appendObu(frame, OBU_TEMPORAL_DELIMITER, {});
    appendObu(frame, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(frame, OBU_FRAME, filler(4000, 12));
    return frame;
}

struct Feed
{
    PacketPool pool;
    VideoReceiver receiver{ SSRC, VideoCodec::VP8 };
    qint64 now = 0;

    static Bytes payload(bool first, bool keyframe, uint8_t tag)
    {
        Bytes bytes = { static_cast<uint8_t>(first ? 0x10 : 0x00), static_cast<uint8_t>(keyframe ? 0x00 : 0x01), tag, tag };
        return bytes;
    }

    void send(uint16_t sequence, uint32_t timestamp, bool first, bool last, bool keyframe, bool retransmission = false)
    {
        PacketPool::Handle packet = pool.acquire();
        const Bytes bytes = payload(first, keyframe, static_cast<uint8_t>(sequence));
        packet->append(bytes);
        receiver.insert(sequence, timestamp, last, std::move(packet), now, retransmission ? VideoReceiver::Arrival::Retransmitted : VideoReceiver::Arrival::FirstTime);
    }

    void sendFrame(int index, uint16_t base, bool keyframe, int skip = -1)
    {
        for (int i = 0; i < 3; i++) {
            if (i != skip)
                send(static_cast<uint16_t>(base + index * 3 + i), 1000u * static_cast<uint32_t>(index), i == 0, i == 2, keyframe);
        }
    }
};

} // namespace

class TestRtpVideo : public QObject
{
    Q_OBJECT
private slots:

    void h264AgainstFfmpeg();
    void h265AgainstFfmpeg();
    void vp8AgainstFfmpeg();
    void av1AgainstFfmpeg();
    void av1ByHand();
    void av1LastObuGetsItsSizeBack();
    void malformedPayloadsAreRejected();

    void encryptedFramesSurviveTheReceiveChain_data();
    void encryptedFramesSurviveTheReceiveChain();

    void keyframeKeepsWhatPrecedesItsParameterSets();
    void pictureWithoutItsParameterSetsIsNotAStart();
    void nothingIsDeliveredBeforeAKeyframe();
    void aKeyframeIsAskedForFiveTimesASecondWhilePacketsCome();
    void framesComeOutInOrder();
    void reorderedPacketsAreHeld();
    void lostPacketIsRequestedAndAccepted();
    void unrepairedLossWaitsForAKeyframe();
    void paddingOnTheStreamLeavesNoGap();
    void sequenceNumbersWrap();
    void aStrayPacketDoesNotRestartTheStream();
    void aSenderThatRestartsItsNumberingIsFollowed();
    void undecodableOutputAsksForAKeyframe();
    void receptionReportCountsLoss();

    void receivedRtcpIsRead();
    void reportsOnWhatThisClientSendsAreRead();
    void receiverReportBytes();
    void nackBytes();
    void pliBytes();

    void transportFeedbackForPacketsInARow();
    void transportFeedbackMarksWhatDidNotArrive();
    void transportFeedbackCarriesLongAndBackwardGaps();
    void transportFeedbackReportsEachPacketOnce();
    void transportFeedbackAcrossTheSequenceWrap();
    void transportFeedbackLeavesOutALateArrival();
};

void TestRtpVideo::h264AgainstFfmpeg()
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = keyframe ? h264Keyframe() : h264DeltaFrame();
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_H264, frame, keyframe);
        QVERIFY(packets.payloads.size() > 2);
        QVERIFY(packets.markers.back());
        QCOMPARE(startsKeyframe(VideoCodec::H264, packets.payloads.front()), keyframe);

        Bytes rebuilt;
        QVERIFY(reassemble(VideoCodec::H264, packets.payloads, rebuilt));
        QVERIFY(rebuilt == frame);
    }
}

void TestRtpVideo::h265AgainstFfmpeg()
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = keyframe ? h265Keyframe() : h265DeltaFrame();
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_HEVC, frame, keyframe);
        QVERIFY(packets.payloads.size() > 2);
        QCOMPARE(startsKeyframe(VideoCodec::H265, packets.payloads.front()), keyframe);

        Bytes rebuilt;
        QVERIFY(reassemble(VideoCodec::H265, packets.payloads, rebuilt));
        QVERIFY(rebuilt == frame);
    }
}

void TestRtpVideo::vp8AgainstFfmpeg()
{
    for (const bool keyframe : { true, false }) {
        const Bytes frame = vp8Frame(keyframe, 4000, 20);
        const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_VP8, frame, keyframe);
        QVERIFY(packets.payloads.size() > 2);
        QCOMPARE(startsKeyframe(VideoCodec::VP8, packets.payloads.front()), keyframe);
        QVERIFY(!startsKeyframe(VideoCodec::VP8, packets.payloads[1]));

        Bytes rebuilt;
        QVERIFY(reassemble(VideoCodec::VP8, packets.payloads, rebuilt));
        QVERIFY(rebuilt == frame);
    }
}

void TestRtpVideo::av1AgainstFfmpeg()
{
    const RtpPayloads packets = packetizeWithFfmpeg(AV_CODEC_ID_AV1, av1Frame(true), true);
    if (packets.payloads.empty())
        QSKIP("this ffmpeg has no AV1 RTP packetizer");

    Bytes rebuilt;
    QVERIFY(reassemble(VideoCodec::AV1, packets.payloads, rebuilt));
    prepareForDecoder(VideoCodec::AV1, rebuilt);
    QVERIFY(rebuilt == av1Frame(false));
}

void TestRtpVideo::av1ByHand()
{
    const RtpPayloads packets = packetizeAv1ByHand(av1Frame(false), true);
    QVERIFY(packets.payloads.size() > 2);
    QVERIFY(startsKeyframe(VideoCodec::AV1, packets.payloads.front()));
    QVERIFY(!startsKeyframe(VideoCodec::AV1, packets.payloads[1]));
    QVERIFY(!startsKeyframe(VideoCodec::AV1, packetizeAv1ByHand(av1Frame(false), false).payloads.front()));

    Bytes expected;
    appendObu(expected, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(expected, OBU_FRAME, filler(4000, 12), false);

    Bytes rebuilt;
    QVERIFY(reassemble(VideoCodec::AV1, packets.payloads, rebuilt));
    QVERIFY(rebuilt == expected);
}

void TestRtpVideo::av1LastObuGetsItsSizeBack()
{
    Bytes frame;
    appendObu(frame, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(frame, OBU_FRAME, filler(300, 12), false);

    prepareForDecoder(VideoCodec::AV1, frame);

    Bytes expected;
    appendObu(expected, OBU_SEQUENCE_HEADER, filler(11, 11));
    appendObu(expected, OBU_FRAME, filler(300, 12));
    QVERIFY(frame == expected);

    prepareForDecoder(VideoCodec::AV1, frame);
    QVERIFY(frame == expected);
}

void TestRtpVideo::malformedPayloadsAreRejected()
{
    Bytes frame;
    const auto assembleOne = [&frame](VideoCodec codec, const Bytes &payload) {
        const std::span<const uint8_t> span(payload);
        return assembleFrame(codec, { &span, 1 }, frame);
    };

    QVERIFY(!assembleOne(VideoCodec::H264, { 0x7C, 0x05, 0xAA }));
    QVERIFY(!assembleOne(VideoCodec::H264, { 0x18, 0x00, 0x09, 0x67, 0x01 }));
    QVERIFY(!assembleOne(VideoCodec::H265, { 0x62, 0x01, 0x13, 0xAA }));
    QVERIFY(!assembleOne(VideoCodec::H265, { 0x60, 0x01, 0x00, 0x09, 0x40, 0x01 }));
    QVERIFY(!assembleOne(VideoCodec::VP8, { 0x00, 0x01, 0x02 }));
    QVERIFY(!assembleOne(VideoCodec::AV1, { 0x80, 0x02, 0x30, 0x01 }));
    QVERIFY(!assembleOne(VideoCodec::AV1, { 0x40, 0x02, 0x30, 0x01 }));
    QVERIFY(!assembleOne(VideoCodec::AV1, { 0x00, 0x09, 0x30, 0x01 }));
    QVERIFY(!assembleOne(VideoCodec::Unknown, { 0x01, 0x02 }));
}

void TestRtpVideo::encryptedFramesSurviveTheReceiveChain_data()
{
    QTest::addColumn<int>("codec");
    QTest::newRow("H264") << static_cast<int>(VideoCodec::H264);
    QTest::newRow("H265") << static_cast<int>(VideoCodec::H265);
    QTest::newRow("VP8") << static_cast<int>(VideoCodec::VP8);
    QTest::newRow("AV1") << static_cast<int>(VideoCodec::AV1);
}

void TestRtpVideo::encryptedFramesSurviveTheReceiveChain()
{
    QFETCH(int, codec);
    const auto videoCodec = static_cast<VideoCodec>(codec);

    Bytes original;
    Bytes expected;
    discord::dave::Codec daveCodec = discord::dave::Codec::Unknown;
    AVCodecID ffmpegCodec = AV_CODEC_ID_NONE;
    switch (videoCodec) {
    case VideoCodec::H264:
        original = expected = h264Keyframe();
        daveCodec = discord::dave::Codec::H264;
        ffmpegCodec = AV_CODEC_ID_H264;
        break;
    case VideoCodec::H265:
        original = expected = h265Keyframe();
        daveCodec = discord::dave::Codec::H265;
        ffmpegCodec = AV_CODEC_ID_HEVC;
        break;
    case VideoCodec::VP8:
        original = expected = vp8Frame(true, 4000, 20);
        daveCodec = discord::dave::Codec::VP8;
        ffmpegCodec = AV_CODEC_ID_VP8;
        break;
    case VideoCodec::AV1:
        original = av1Frame(true);
        expected = av1Frame(false);
        daveCodec = discord::dave::Codec::AV1;
        break;
    case VideoCodec::Unknown:
        break;
    }

    const Bytes encrypted = encryptFrame(daveCodec, SSRC, original);
    QVERIFY(!encrypted.empty());
    QVERIFY(encrypted != original);

    const RtpPayloads packets = videoCodec == VideoCodec::AV1 ? packetizeAv1ByHand(encrypted, true) : packetizeWithFfmpeg(ffmpegCodec, encrypted, true);
    QVERIFY(packets.payloads.size() > 2);

    Bytes received;
    QVERIFY(reassemble(videoCodec, packets.payloads, received));
    QVERIFY(received == encrypted);

    Bytes decrypted;
    QVERIFY(decryptFrame(received, decrypted));
    prepareForDecoder(videoCodec, decrypted);
    QVERIFY(decrypted == expected);
}

void TestRtpVideo::keyframeKeepsWhatPrecedesItsParameterSets()
{
    const std::vector<Bytes> packets = {
        { 0x41, 0x80, 0x11 },
        { 0x09, 0xF0 },
        { 0x18, 0x00, 0x03, 0x67, 0x42, 0x00, 0x00, 0x02, 0x68, 0xCE },
        { 0x65, 0x88, 0x84 },
    };
    const std::vector<uint32_t> timestamps = { 3000, 6000, 6000, 6000 };

    PacketPool pool;
    VideoReceiver receiver(SSRC, VideoCodec::H264);
    for (size_t i = 0; i < packets.size(); i++) {
        PacketPool::Handle packet = pool.acquire();
        packet->append(packets[i]);
        receiver.insert(static_cast<uint16_t>(500 + i), timestamps[i], i == 0 || i + 1 == packets.size(), std::move(packet), 0);
    }

    const std::vector<EncodedVideoFrame> frames = receiver.takeFrames();
    QCOMPARE(frames.size(), size_t(1));
    QVERIFY(frames[0].keyframe);
    const Bytes expected = { 0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x67, 0x42, 0x00, 0, 0, 0, 1, 0x68, 0xCE, 0, 0, 0, 1, 0x65, 0x88, 0x84 };
    QVERIFY(frames[0].data == expected);

    QVERIFY(startsKeyframe(VideoCodec::H264, Bytes{ 0x18, 0x00, 0x02, 0x09, 0xF0, 0x00, 0x03, 0x67, 0x42, 0x00 }));
    QVERIFY(!startsKeyframe(VideoCodec::H264, Bytes{ 0x18, 0x00, 0x02, 0x09, 0xF0, 0x00, 0x03, 0x41, 0x80, 0x00 }));
    QVERIFY(startsKeyframe(VideoCodec::H265, Bytes{ 0x60, 0x01, 0x00, 0x03, 0x46, 0x01, 0x50, 0x00, 0x04, 0x40, 0x01, 0x0C, 0x01 }));
}

void TestRtpVideo::pictureWithoutItsParameterSetsIsNotAStart()
{
    const Bytes parameterSets = { 0x18, 0x00, 0x03, 0x67, 0x42, 0x00, 0x00, 0x02, 0x68, 0xCE };
    const Bytes picture = { 0x65, 0x88, 0x84 };
    const Bytes delta = { 0x41, 0x80, 0x11 };
    const auto send = [](VideoReceiver &receiver, PacketPool &pool, uint16_t sequence, uint32_t timestamp, bool marker, const Bytes &bytes) {
        PacketPool::Handle packet = pool.acquire();
        packet->append(bytes);
        receiver.insert(sequence, timestamp, marker, std::move(packet), 0);
    };

    PacketPool pool;
    VideoReceiver late(SSRC, VideoCodec::H264);
    send(late, pool, 501, 6000, true, picture);
    QVERIFY(late.takeFrames().empty());
    QVERIFY(late.poll(0).keyframeRequest);

    send(late, pool, 510, 9000, false, parameterSets);
    send(late, pool, 511, 9000, true, picture);
    QCOMPARE(late.takeFrames().size(), size_t(1));

    VideoReceiver bare(SSRC, VideoCodec::H264);
    send(bare, pool, 700, 3000, true, delta);
    send(bare, pool, 701, 6000, true, picture);
    const std::vector<EncodedVideoFrame> frames = bare.takeFrames();
    QCOMPARE(frames.size(), size_t(1));
    QVERIFY(frames[0].keyframe);
}

void TestRtpVideo::aKeyframeIsAskedForFiveTimesASecondWhilePacketsCome()
{
    QCOMPARE(VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS, 200);
    QCOMPARE(VideoReceiver::SILENT_AFTER_MS, 5000);

    Feed feed;
    QVERIFY(!feed.receiver.poll(0).keyframeRequest);

    feed.now = 1000;
    feed.sendFrame(0, 100, false);
    int requests = 0;
    for (qint64 now = 1000; now < 2000; now += 10)
        requests += feed.receiver.poll(now).keyframeRequest ? 1 : 0;
    QCOMPARE(requests, 5);

    QVERIFY(feed.receiver.poll(1000 + VideoReceiver::SILENT_AFTER_MS - 1).keyframeRequest);
    for (qint64 now = 1000 + VideoReceiver::SILENT_AFTER_MS; now < 20000; now += 100)
        QVERIFY(!feed.receiver.poll(now).keyframeRequest);

    feed.now = 20000;
    feed.sendFrame(1, 100, false);
    QVERIFY(feed.receiver.poll(20000).keyframeRequest);
    QVERIFY(feed.receiver.takeFrames().empty());
}

void TestRtpVideo::nothingIsDeliveredBeforeAKeyframe()
{
    Feed feed;
    feed.sendFrame(0, 100, false);
    feed.sendFrame(1, 100, false);
    QVERIFY(feed.receiver.takeFrames().empty());

    VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    QVERIFY(feedback.keyframeRequest);
    QVERIFY(feedback.nacks.empty());

    QVERIFY(!feed.receiver.poll(feed.now + 20).keyframeRequest);
    QVERIFY(feed.receiver.poll(feed.now + VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS).keyframeRequest);

    feed.sendFrame(2, 100, true);
    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    QCOMPARE(frames.size(), size_t(1));
    QVERIFY(frames[0].keyframe);
    QCOMPARE(frames[0].rtpTimestamp, 2000u);
    QVERIFY(!feed.receiver.poll(feed.now + 5000).keyframeRequest);
}

void TestRtpVideo::framesComeOutInOrder()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false);
    feed.sendFrame(2, 100, false);

    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    QCOMPARE(frames.size(), size_t(3));
    QVERIFY(frames[0].keyframe);
    QVERIFY(!frames[1].keyframe);
    QCOMPARE(frames[1].rtpTimestamp, 1000u);
    QCOMPARE(frames[2].rtpTimestamp, 2000u);
    QCOMPARE(frames[0].data.size(), size_t(9));
    QCOMPARE(frames[0].data[1], uint8_t(100));
    QCOMPARE(frames[0].data[4], uint8_t(101));
    QVERIFY(feed.receiver.takeFrames().empty());
}

void TestRtpVideo::reorderedPacketsAreHeld()
{
    Feed feed;
    feed.send(102, 0, false, true, true);
    feed.send(101, 0, false, false, true);
    QVERIFY(feed.receiver.takeFrames().empty());
    feed.send(100, 0, true, false, true);

    Feed late;
    late.sendFrame(0, 100, true);
    QCOMPARE(late.receiver.takeFrames().size(), size_t(1));
    late.sendFrame(2, 100, false);
    QVERIFY(late.receiver.takeFrames().empty());
    late.sendFrame(1, 100, false);
    QCOMPARE(late.receiver.takeFrames().size(), size_t(2));

    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));
}

void TestRtpVideo::lostPacketIsRequestedAndAccepted()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 1);
    feed.sendFrame(2, 100, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    QVERIFY(feed.receiver.poll(feed.now).nacks.empty());

    feed.now += VideoReceiver::REORDER_GRACE_MS;
    VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    QCOMPARE(feedback.nacks, std::vector<uint16_t>{ 104 });
    QVERIFY(!feedback.keyframeRequest);

    QVERIFY(feed.receiver.poll(feed.now + 20).nacks.empty());
    feed.now += VideoReceiver::NACK_RETRY_MS;
    QCOMPARE(feed.receiver.poll(feed.now).nacks, std::vector<uint16_t>{ 104 });

    feed.send(104, 1000, false, false, false, true);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(2));
    QCOMPARE(feed.receiver.stats().retransmitted, quint64(1));
    QVERIFY(feed.receiver.poll(feed.now + 5000).nacks.empty());
}

void TestRtpVideo::unrepairedLossWaitsForAKeyframe()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 0);
    feed.sendFrame(2, 100, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    feed.now += VideoReceiver::NACK_GIVE_UP_MS + 1;
    const VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    QVERIFY(feedback.keyframeRequest);
    QVERIFY(feedback.nacks.empty());

    feed.sendFrame(3, 100, false);
    QVERIFY(feed.receiver.takeFrames().empty());

    feed.sendFrame(4, 100, true);
    feed.sendFrame(5, 100, false);
    const std::vector<EncodedVideoFrame> frames = feed.receiver.takeFrames();
    QCOMPARE(frames.size(), size_t(2));
    QVERIFY(frames[0].keyframe);
    QCOMPARE(frames[0].rtpTimestamp, 4000u);
}

void TestRtpVideo::paddingOnTheStreamLeavesNoGap()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    for (int i = 0; i < 3; i++)
        feed.send(static_cast<uint16_t>(104 + i), 1000, i == 0, i == 2, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    feed.receiver.skip(103, 0, feed.now);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    feed.now += VideoReceiver::NACK_GIVE_UP_MS + 1;
    const VideoReceiver::Feedback feedback = feed.receiver.poll(feed.now);
    QVERIFY(feedback.nacks.empty());
    QVERIFY(!feedback.keyframeRequest);
    QCOMPARE(feed.receiver.stats().resyncs, quint64(0));
}

void TestRtpVideo::sequenceNumbersWrap()
{
    Feed feed;
    const uint16_t base = 65530;
    for (int index = 0; index < 5; index++)
        feed.sendFrame(index, base, index == 0);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(5));

    feed.sendFrame(5, base, false, 1);
    feed.sendFrame(6, base, false);
    feed.now += VideoReceiver::REORDER_GRACE_MS;
    QCOMPARE(feed.receiver.poll(feed.now).nacks, std::vector<uint16_t>{ static_cast<uint16_t>(base + 16) });
}

void TestRtpVideo::aStrayPacketDoesNotRestartTheStream()
{
    Feed feed;
    feed.sendFrame(0, 20000, true);
    feed.sendFrame(1, 20000, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(2));

    feed.send(100, 0, true, true, false);
    QCOMPARE(feed.receiver.stats().strays, quint64(1));
    QCOMPARE(feed.receiver.stats().resyncs, quint64(0));

    feed.sendFrame(2, 20000, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));
    QVERIFY(!feed.receiver.poll(feed.now).keyframeRequest);
}

void TestRtpVideo::aSenderThatRestartsItsNumberingIsFollowed()
{
    Feed feed;
    feed.sendFrame(0, 20000, true);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    feed.sendFrame(1, 100, false);
    QCOMPARE(feed.receiver.stats().strays, quint64(1));
    QCOMPARE(feed.receiver.stats().resyncs, quint64(1));
    QVERIFY(feed.receiver.takeFrames().empty());
    QVERIFY(feed.receiver.poll(feed.now).keyframeRequest);

    feed.sendFrame(2, 100, true);
    feed.sendFrame(3, 100, false);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(2));
}

void TestRtpVideo::undecodableOutputAsksForAKeyframe()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));
    QVERIFY(!feed.receiver.poll(feed.now).keyframeRequest);

    feed.receiver.requestKeyframe();
    QVERIFY(feed.receiver.poll(feed.now).keyframeRequest);
    feed.sendFrame(1, 100, false);
    QVERIFY(feed.receiver.takeFrames().empty());
    feed.sendFrame(2, 100, true);
    QCOMPARE(feed.receiver.takeFrames().size(), size_t(1));

    feed.receiver.requestKeyframe();
    QVERIFY(!feed.receiver.poll(feed.now + 20).keyframeRequest);
    QVERIFY(feed.receiver.poll(feed.now + VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS).keyframeRequest);
}

void TestRtpVideo::receptionReportCountsLoss()
{
    Feed feed;
    feed.sendFrame(0, 100, true);
    feed.sendFrame(1, 100, false, 1);
    feed.sendFrame(2, 100, false);
    feed.sendFrame(3, 100, false, 2);

    ReceptionReport report = feed.receiver.receptionReport();
    QCOMPARE(report.ssrc, SSRC);
    QCOMPARE(report.highestSequence & 0xFFFFu, 110u);
    QCOMPARE(report.cumulativeLost, 1);
    // one of the eleven packets expected so far, as a fraction of 256
    QCOMPARE(int(report.fractionLost), 256 / 11);

    feed.sendFrame(4, 100, false);
    report = feed.receiver.receptionReport();
    QCOMPARE(report.cumulativeLost, 2);
    QCOMPARE(int(report.fractionLost), 256 / 4);
}

void TestRtpVideo::receivedRtcpIsRead()
{
    const auto bytes = [](const char *hex) {
        const QByteArray raw = QByteArray::fromHex(hex);
        return Bytes(raw.begin(), raw.end());
    };

    // the NTP time is ee73cd84.7cbccc93
    ReceivedRtcp received = parseRtcp(bytes("80c800060000008aee73cd847cbccc930512806400000053000129f0"));
    QCOMPARE(received.senderReports.size(), size_t(1));
    QCOMPARE(received.senderReports[0].ssrc, 0x8Au);
    QCOMPARE(received.senderReports[0].compactNtp, 0xCD847CBCu);
    QVERIFY(!received.inboundEstimate);

    // Discord's estimate: mantissa 0x22560 under exponent 6
    received = parseRtcp(bytes("81c90007000003b1000003b100000000000000000000003b0000000000000000" "81cc00050000008a3153494401020304050607081a256000"));
    QVERIFY(received.senderReports.empty());
    QVERIFY(received.inboundEstimate);
    QCOMPARE(received.inboundEstimate->bps, quint64(9000960));
    QCOMPARE(received.inboundEstimate->lossFraction, 0.0);

    // exponent 7, and a quarter of the packets lost
    received = parseRtcp(bytes("81cc00050000008a3153494401020304050607081efaf040"));
    QCOMPARE(received.inboundEstimate->bps, quint64(24999936));
    QCOMPARE(received.inboundEstimate->lossFraction, 0.25);

    QVERIFY(!parseRtcp(bytes("81cc00050000008a5853494401020304050607081efaf040")).inboundEstimate);
    QVERIFY(!parseRtcp(bytes("81cc00050000008a31534944010203")).inboundEstimate);
    QVERIFY(parseRtcp(bytes("80c800ff0000008aee73cd847cbccc93")).senderReports.empty());
    QVERIFY(parseRtcp({}).senderReports.empty());
}

void TestRtpVideo::reportsOnWhatThisClientSendsAreRead()
{
    ReceptionReport sent;
    sent.ssrc = 7713;
    sent.fractionLost = 64;
    sent.cumulativeLost = -3;
    sent.highestSequence = 0x00010457;
    sent.jitter = 12;

    uint8_t packet[64];
    const ReceptionReport reports[] = { sent };
    const int size = writeReceiverReport(packet, 99, reports);
    QVERIFY(size > 0);

    const ReceivedRtcp received = parseRtcp({ packet, static_cast<size_t>(size) });
    QCOMPARE(received.receptionReports.size(), size_t(1));
    QCOMPARE(received.receptionReports[0].ssrc, sent.ssrc);
    QCOMPARE(received.receptionReports[0].fractionLost, sent.fractionLost);
    QCOMPARE(received.receptionReports[0].cumulativeLost, sent.cumulativeLost);
    QCOMPARE(received.receptionReports[0].highestSequence, sent.highestSequence);
    QCOMPARE(received.receptionReports[0].jitter, sent.jitter);
}

void TestRtpVideo::receiverReportBytes()
{
    uint8_t buffer[64];

    QCOMPARE(writeReceiverReport(buffer, 0x01020304, {}), 8);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), 8).toHex(), QByteArray("80c9000101020304"));

    ReceptionReport report;
    report.ssrc = 0x0A0B0C0D;
    report.fractionLost = 0x40;
    report.cumulativeLost = 0x010203;
    report.highestSequence = 0x00011234;
    report.jitter = 0x55;
    const int size = writeReceiverReport(buffer, 0x01020304, { &report, 1 });
    QCOMPARE(size, 32);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), size).toHex(), QByteArray("81c90007010203040a0b0c0d4001020300011234000000550000000000000000"));

    QCOMPARE(writeReceiverReport(std::span<uint8_t>(buffer, 31), 1, { &report, 1 }), -1);
}

void TestRtpVideo::nackBytes()
{
    uint8_t buffer[64];

    // 100 and the two after it share an entry; 200 is too far away and gets its own
    const std::vector<uint16_t> lost = { 100, 101, 116, 200 };
    const int size = writeNack(buffer, 0x01020304, 0x0A0B0C0D, lost);
    QCOMPARE(size, 20);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), size).toHex(), QByteArray("81cd0004010203040a0b0c0d0064800100c80000"));

    const std::vector<uint16_t> wrapping = { 65535, 0, 2 };
    QCOMPARE(writeNack(buffer, 1, 2, wrapping), 16);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 4).toHex(), QByteArray("ffff0005"));

    QCOMPARE(writeNack(buffer, 1, 2, {}), -1);
}

void TestRtpVideo::pliBytes()
{
    uint8_t buffer[16];
    QCOMPARE(writePli(buffer, 0x01020304, 0x0A0B0C0D), 12);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), 12).toHex(), QByteArray("81ce0002010203040a0b0c0d"));
    QCOMPARE(writePli(std::span<uint8_t>(buffer, 11), 1, 2), -1);
}

void TestRtpVideo::transportFeedbackForPacketsInARow()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    QVERIFY(!feedback.hasPending());
    QCOMPARE(feedback.write(buffer, 1, 2), 0);

    // a millisecond apart, the first 2 ms after a 64 ms boundary: one run of "received", and deltas of
    // 8, 4 and 4 quarter-milliseconds
    feedback.onPacket(100, 322000);
    feedback.onPacket(101, 323000);
    feedback.onPacket(102, 324000);
    QVERIFY(feedback.hasPending());

    const int size = feedback.write(buffer, 0x01020304, 0x0A0B0C0D);
    QCOMPARE(size, 28);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), size).toHex(), QByteArray("afcd0006010203040a0b0c0d006400030000050020030804040000" "03"));
    QVERIFY(!feedback.hasPending());

    for (uint16_t i = 0; i < 20; i++)
        feedback.onPacket(static_cast<uint16_t>(200 + i), 640000 + i * 250);
    QCOMPARE(feedback.write(buffer, 1, 2), 44);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 10).toHex(), QByteArray("00c8001400000a012014"));
}

void TestRtpVideo::transportFeedbackMarksWhatDidNotArrive()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(10, 640000);
    feedback.onPacket(12, 641000);

    // received, missing, received: spelled out two bits each, and only two deltas
    const int size = feedback.write(buffer, 0x01020304, 0x0A0B0C0D);
    QCOMPARE(size, 24);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer), size).toHex(), QByteArray("8fcd0005010203040a0b0c0d000a000300000a00d1000004"));
}

void TestRtpVideo::transportFeedbackCarriesLongAndBackwardGaps()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    // the second packet 100 ms behind the first, the third arriving before the second
    feedback.onPacket(1, 0);
    feedback.onPacket(2, 100000);
    feedback.onPacket(3, 99000);

    const int size = feedback.write(buffer, 1, 2);
    QCOMPARE(size, 28);
    // small, large, large; then 0, +400 and -4 quarter-milliseconds
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 16).toHex(), QByteArray("0001000300000000da00000190fffc01"));
}

void TestRtpVideo::transportFeedbackReportsEachPacketOnce()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(5, 64000);
    QCOMPARE(feedback.write(buffer, 1, 2), 24);
    QCOMPARE(buffer[19], uint8_t(0));

    feedback.onPacket(6, 128000);
    feedback.onPacket(7, 128250);
    QCOMPARE(feedback.write(buffer, 1, 2), 24);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 12).toHex(), QByteArray("000600020000020120020001"));
    QCOMPARE(feedback.write(buffer, 1, 2), 0);

    feedback.onPacket(8, 192000);
    QCOMPARE(feedback.write(std::span<uint8_t>(buffer, 20), 1, 2), -1);
    QVERIFY(feedback.hasPending());
    QCOMPARE(feedback.write(buffer, 1, 2), 24);
}

void TestRtpVideo::transportFeedbackAcrossTheSequenceWrap()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(65535, 64000);
    feedback.onPacket(0, 64250);
    feedback.onPacket(1, 64500);

    QCOMPARE(feedback.write(buffer, 1, 2), 28);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 4).toHex(), QByteArray("ffff0003"));
}

void TestRtpVideo::transportFeedbackLeavesOutALateArrival()
{
    uint8_t buffer[128];
    TransportFeedback feedback;
    feedback.onPacket(100, 64000);
    feedback.onPacket(102, 64250);
    QCOMPARE(feedback.write(buffer, 1, 2), 24);

    // the next report is of 103 alone, not of 101 with 102 missing
    feedback.onPacket(101, 64500);
    feedback.onPacket(103, 64750);
    QCOMPARE(feedback.write(buffer, 1, 2), 24);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer) + 12, 4).toHex(), QByteArray("00670001"));
    QVERIFY(!feedback.hasPending());
}

QTEST_GUILESS_MAIN(TestRtpVideo)
#include "tst_RtpVideo.moc"
