
#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"
#include "Discord/Voice/VideoSendStream.hpp"
#include "VideoFrameHelpers.hpp"

#include <bytes/bytes.h>
#include <dave/dave_interfaces.h>

#include <QTest>
#include <QtEndian>

#include <array>
#include <optional>
#include <random>

using namespace Acheron::Discord::Voice;

namespace {

using Bytes = std::vector<uint8_t>;
using Payloads = std::vector<Bytes>;
using namespace VideoFrameHelpers;

constexpr uint32_t SSRC = 0x1234;
constexpr uint32_t RTX_SSRC = 0x1299;
constexpr uint8_t PAYLOAD_TYPE = 101;
constexpr uint8_t RTX_PAYLOAD_TYPE = 102;

constexpr int PACKET_OVERHEAD = 12 + 4 + 12;
constexpr int MAX_PAYLOAD = 1200 - PACKET_OVERHEAD - 2;
constexpr int VP8_DESCRIPTOR = 4;
constexpr int VP8_ROOM = MAX_PAYLOAD - VP8_DESCRIPTOR;
constexpr int FU_HEADER = 2;

constexpr int NAL_TYPE_MASK = 0x1F;
constexpr int NAL_SLICE = 0x41;
constexpr int NAL_IDR = 0x65;
constexpr int NAL_SPS = 0x67;
constexpr int NAL_PPS = 0x68;
constexpr int STAP_A = 24;
constexpr int FU_A = 28;

// the type above a layer id of zero, then the temporal id plus one
using H265Header = std::array<uint8_t, 2>;
constexpr H265Header H265_SLICE = { 1 << 1, 1 };
constexpr H265Header H265_IDR = { 19 << 1, 1 };
constexpr H265Header H265_VPS = { 32 << 1, 1 };
constexpr H265Header H265_SPS = { 33 << 1, 1 };
constexpr H265Header H265_PPS = { 34 << 1, 1 };
constexpr int H265_AP = 48;
constexpr int H265_FU = 49;
constexpr int H265_NAL_HEADER = 2;
constexpr int H265_FU_HEADER = 3;
constexpr int H265_FU_ROOM = MAX_PAYLOAD - H265_FU_HEADER;

constexpr uint8_t OBU_SEQUENCE_HEADER = 1;
constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;
constexpr uint8_t OBU_FRAME_HEADER = 3;
constexpr uint8_t OBU_TILE_GROUP = 4;
constexpr uint8_t OBU_METADATA = 5;
constexpr uint8_t OBU_FRAME = 6;
constexpr uint8_t OBU_TILE_LIST = 8;
constexpr uint8_t OBU_PADDING = 15;
// the element count lies between these bits
constexpr int AV1_CONTINUES_PREVIOUS = 0x80;
constexpr int AV1_CONTINUES_IN_NEXT = 0x40;
constexpr int AV1_NEW_SEQUENCE = 0x08;
constexpr int AV1_ROOM = MAX_PAYLOAD - 1;

constexpr int av1Elements(int count)
{
    return count << 4;
}

constexpr qint64 MS = 1000;
constexpr qint64 SECOND = 1000 * MS;

Bytes vp8Frame(bool keyframe, size_t size, uint32_t seed)
{
    Bytes frame = filler(size, seed);
    frame[0] = static_cast<uint8_t>(keyframe ? (frame[0] & ~1) : (frame[0] | 1));
    return frame;
}

void appendNal(Bytes &frame, uint8_t header, size_t size, uint32_t seed, int startCodeSize = 4)
{
    frame.insert(frame.end(), static_cast<size_t>(startCodeSize - 1), 0);
    frame.push_back(1);
    frame.push_back(header);
    Bytes body = filler(size - 1, seed);
    if (!body.empty())
        body[0] |= 0x80;
    frame.insert(frame.end(), body.begin(), body.end());
}

Bytes h264Keyframe(size_t pictureSize, int startCodeSize = 4)
{
    Bytes frame;
    appendNal(frame, NAL_SPS, 21, 1, startCodeSize);
    appendNal(frame, NAL_PPS, 7, 2, startCodeSize);
    appendNal(frame, NAL_IDR, pictureSize, 3, startCodeSize);
    return frame;
}

void appendH265Nal(Bytes &frame, H265Header header, size_t size, uint32_t seed, int startCodeSize = 4)
{
    frame.insert(frame.end(), static_cast<size_t>(startCodeSize - 1), 0);
    frame.push_back(1);
    frame.insert(frame.end(), header.begin(), header.end());
    Bytes body = filler(size - H265_NAL_HEADER, seed);
    if (!body.empty())
        body[0] |= 0x80;
    frame.insert(frame.end(), body.begin(), body.end());
}

Bytes h265Keyframe(size_t pictureSize, int startCodeSize = 4)
{
    Bytes frame;
    appendH265Nal(frame, H265_VPS, 24, 4, startCodeSize);
    appendH265Nal(frame, H265_SPS, 40, 5, startCodeSize);
    appendH265Nal(frame, H265_PPS, 9, 6, startCodeSize);
    appendH265Nal(frame, H265_IDR, pictureSize, 7, startCodeSize);
    return frame;
}

void appendLeb128(Bytes &out, size_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

struct Obu
{
    uint8_t type = 0;
    size_t payloadSize = 0;
    bool extension = false;
};

enum class LastObu {
    Sized,
    Unsized,
};

Bytes av1Frame(const std::vector<Obu> &obus, LastObu last = LastObu::Sized)
{
    Bytes frame;
    for (size_t i = 0; i < obus.size(); i++) {
        const Obu &obu = obus[i];
        const bool sized = last == LastObu::Sized || i + 1 < obus.size();
        frame.push_back(static_cast<uint8_t>((obu.type << 3) | (obu.extension ? 0x04 : 0) | (sized ? 0x02 : 0)));
        if (obu.extension)
            frame.push_back(0x48);
        if (sized)
            appendLeb128(frame, obu.payloadSize);
        const Bytes payload = filler(obu.payloadSize, 1000u * obu.type + static_cast<uint32_t>(obu.payloadSize));
        frame.insert(frame.end(), payload.begin(), payload.end());
    }
    return frame;
}

const std::vector<Obu> AV1_KEYFRAME_SENT = { { OBU_TEMPORAL_DELIMITER, 0 }, { OBU_SEQUENCE_HEADER, 11 }, { OBU_FRAME, 4000 } };
const std::vector<Obu> AV1_KEYFRAME_KEPT = { { OBU_SEQUENCE_HEADER, 11 }, { OBU_FRAME, 4000 } };

bool av1ComesBackAs(const std::vector<Obu> &kept, const Payloads &payloads)
{
    Bytes frame;
    if (!reassemble(VideoCodec::AV1, payloads, frame) || frame != av1Frame(kept, LastObu::Unsized))
        return false;
    prepareForDecoder(VideoCodec::AV1, frame);
    return frame == av1Frame(kept);
}

discord::dave::Codec daveCodec(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return discord::dave::Codec::H264;
    case VideoCodec::H265:
        return discord::dave::Codec::H265;
    case VideoCodec::VP8:
        return discord::dave::Codec::VP8;
    case VideoCodec::AV1:
        return discord::dave::Codec::AV1;
    case VideoCodec::Unknown:
        break;
    }
    return discord::dave::Codec::Unknown;
}

bool noneLargerThan(const Payloads &payloads, size_t limit)
{
    return std::all_of(payloads.begin(), payloads.end(), [limit](const Bytes &payload) { return !payload.empty() && payload.size() <= limit; });
}

struct Written
{
    RtpHeader header;
    int clearSize = 0;
    int size = 0;
    Bytes extensions;
    Bytes payload;

    [[nodiscard]] Bytes extension(uint8_t id) const
    {
        const std::span<const uint8_t> value = findRtpExtension(extensions, id);
        return Bytes(value.begin(), value.end());
    }

    [[nodiscard]] uint16_t transportSequence() const
    {
        const Bytes value = extension(TRANSPORT_SEQUENCE_EXTENSION);
        return value.size() == 2 ? qFromBigEndian<uint16_t>(value.data()) : 0;
    }

    [[nodiscard]] uint32_t sendTime() const
    {
        const Bytes value = extension(ABS_SEND_TIME_EXTENSION);
        return value.size() == 3 ? (uint32_t(value[0]) << 16) | (uint32_t(value[1]) << 8) | value[2] : 0xFFFFFFFF;
    }
};

std::optional<Written> takeApart(const PacketBuffer &packet)
{
    const std::optional<ParsedRtp> rtp = parseRtp(packet.bytes());
    if (!rtp || packet.size() < rtp->clearSize + rtp->extensionSize)
        return std::nullopt;

    Written written;
    written.header = rtp->header;
    written.clearSize = rtp->clearSize;
    written.size = packet.size();
    const std::span<const uint8_t> body = packet.bytes().subspan(static_cast<size_t>(rtp->clearSize));
    written.extensions.assign(body.begin(), body.begin() + rtp->extensionSize);
    written.payload.assign(body.begin() + rtp->extensionSize, body.end());
    return written;
}

std::vector<Written> interval(VideoSendStream &sender, qint64 nowUs)
{
    std::vector<Written> out;
    PacketBuffer packet;
    sender.startInterval(nowUs);
    while (sender.writeNext(nowUs, packet)) {
        if (const std::optional<Written> written = takeApart(packet))
            out.push_back(*written);
    }
    return out;
}

std::vector<Written> drain(VideoSendStream &sender, qint64 nowUs)
{
    std::vector<Written> out;
    PacketBuffer packet;
    qint64 paceUs = nowUs;
    for (int intervals = 0; sender.hasWaiting() && intervals < 10000; intervals++) {
        sender.startInterval(paceUs);
        while (sender.writeNext(nowUs, packet)) {
            if (const std::optional<Written> written = takeApart(packet))
                out.push_back(*written);
        }
        paceUs += VideoSendStream::BURST_MS * MS;
    }
    return out;
}

qint64 bytesIn(const std::vector<Written> &packets)
{
    qint64 bytes = 0;
    for (const Written &packet : packets)
        bytes += packet.size;
    return bytes;
}

double paceOf(int bitrate, double factor)
{
    return bitrate * factor / 8000.0;
}

double burstOf(int bitrate, double factor)
{
    return paceOf(bitrate, factor) * 40;
}

void tellRoundTrip(VideoSendStream &sender, int milliseconds)
{
    constexpr uint64_t NTP = uint64_t(3976214400) << 32;
    uint8_t buffer[64];
    QVERIFY(sender.writeSenderReport(buffer, 0, NTP) > 0);
    ReceptionReport report;
    report.ssrc = SSRC;
    report.lastSenderReport = compactNtp(NTP);
    sender.onReceptionReport(report, milliseconds * MS);
    QCOMPARE(sender.stats().roundTripMs, std::optional<int>(milliseconds));
}

Payloads payloadsOf(const std::vector<Written> &packets)
{
    Payloads payloads;
    for (const Written &packet : packets)
        payloads.push_back(packet.payload);
    return payloads;
}

constexpr VideoSendStream::Numbering FIRST{ 65533, 500, 65534, 0x7FFF };

VideoSendStream vp8Sender()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(VideoCodec::VP8, PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    return sender;
}

void append32(Bytes &out, uint32_t value)
{
    uint8_t bytes[4];
    qToBigEndian(value, bytes);
    out.insert(out.end(), bytes, bytes + 4);
}

void append16(Bytes &out, uint16_t value)
{
    uint8_t bytes[2];
    qToBigEndian(value, bytes);
    out.insert(out.end(), bytes, bytes + 2);
}

Bytes rtcpPacket(uint8_t countOrFormat, uint8_t type, const Bytes &body)
{
    Bytes packet = { static_cast<uint8_t>(0x80 | countOrFormat), type };
    append16(packet, static_cast<uint16_t>(body.size() / 4));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

} // namespace

class TestVideoSend : public QObject
{
    Q_OBJECT
private slots:
    void vp8FramesComeBackWhole_data();
    void vp8FramesComeBackWhole();
    void vp8PacketsOfAFrameShareItsPictureId();
    void h264UnitsComeBackWhole_data();
    void h264UnitsComeBackWhole();
    void h264KeyframeAggregatesItsParameterSets();
    void h264KeyframeThatFitsIsOneAggregate();
    void h264ShortStartCodesComeBackLong();
    void h265UnitsComeBackWhole_data();
    void h265UnitsComeBackWhole();
    void h265KeyframeAggregatesItsParameterSets();
    void h265KeyframeThatFitsIsOneAggregate();
    void h265HeadersKeepLayerAndTemporalId();
    void av1ObusComeBackWhole_data();
    void av1ObusComeBackWhole();
    void av1HeaderCountsUpToThreeElements();
    void av1FrameAcrossPacketsSaysWhatContinues();
    void av1ObuSharesThePacketAnotherEndsIn();
    void av1ObusThatAreNotSentAreLeftOut();
    void av1LastObuMayComeWithoutItsSize();
    void framesComeBackWholeAtAnyPacketSize_data();
    void framesComeBackWholeAtAnyPacketSize();
    void whatCannotBePacketizedGivesNothing();
    void encryptedFramesSurvivePacketizing_data();
    void encryptedFramesSurvivePacketizing();
    void encryptWritesPastTheBoundItPromises();

    void packetsAreNumberedMarkedAndExtended();
    void theSendTimeIsSecondsInFixedPoint();
    void noPacketIsLargerThanTheLimit_data();
    void noPacketIsLargerThanTheLimit();
    void aFrameLeavesAtThePaceOfItsBitrate_data();
    void aFrameLeavesAtThePaceOfItsBitrate();
    void aFrameWithinTheBurstLeavesAtOnce();
    void aBacklogLeavesWithinTheQueueTime();
    void aLostPacketIsSentAgainOnTheRetransmissionStream();
    void retransmissionsGoAheadOfNewPackets();
    void whatWasNeverSentIsNotSentAgain();
    void aPacketWaitingToGoAgainIsNotQueuedTwice();
    void aPacketIsNotSentAgainWithinARoundTrip();
    void repairsTakeNoMoreThanTheBitrate();
    void aSparseHistoryLastsThreeSeconds();
    void aFullHistoryEndsAfterASecond();
    void theHistoryOutlastsThreeRoundTrips();
    void withoutARetransmissionPayloadTypeNothingIsSentAgain();
    void changingTheCodecKeepsTheNumbering();
    void anyCodecCanFollowAnyOther();
    void clearingForgetsWhatWaited();
    void framesAreRefusedWithoutACodec();

    void theSenderReportStatesWhatWasSent();
    void aReceptionReportGivesLossAndRoundTrip();
    void feedbackForTheSenderIsRead();
    void theReceiverCountIsRead();
    void theReceiverEstimateIsRead();
    void aVideoResetRequestIsRead();
};

void TestVideoSend::vp8FramesComeBackWhole_data()
{
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("packets");

    QTest::newRow("one byte") << 1 << 1;
    QTest::newRow("exactly one packet") << VP8_ROOM << 1;
    QTest::newRow("one byte over") << VP8_ROOM + 1 << 2;
    QTest::newRow("several packets") << 5 * VP8_ROOM + 300 << 6;
    QTest::newRow("a large key frame") << 250000 << (250000 + VP8_ROOM - 1) / VP8_ROOM;
}

void TestVideoSend::vp8FramesComeBackWhole()
{
    QFETCH(int, size);
    QFETCH(int, packets);

    const Bytes frame = vp8Frame(true, static_cast<size_t>(size), 20);
    VideoPacketizer packetizer(VideoCodec::VP8);
    const Payloads payloads = packetizer.packetize(frame, true, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), packets);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));
    const auto [smallest, largest] = std::minmax_element(payloads.begin(), payloads.end(), [](const Bytes &a, const Bytes &b) { return a.size() < b.size(); });
    QVERIFY(largest->size() - smallest->size() <= 1);

    QVERIFY(startsKeyframe(VideoCodec::VP8, payloads.front()));
    for (size_t i = 0; i < payloads.size(); i++)
        QCOMPARE((payloads[i][0] & 0x10) != 0, i == 0);

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::VP8, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::vp8PacketsOfAFrameShareItsPictureId()
{
    const auto pictureId = [](const Bytes &payload) { return ((payload[2] & 0x7F) << 8) | payload[3]; };

    VideoPacketizer packetizer(VideoCodec::VP8, 0x7FFE);
    for (int expected : { 0x7FFE, 0x7FFF, 0, 1 }) {
        const Payloads payloads = packetizer.packetize(vp8Frame(false, 3000, 21), false, MAX_PAYLOAD);
        QCOMPARE(payloads.size(), size_t(3));
        for (const Bytes &payload : payloads) {
            QCOMPARE(payload[0] & 0xEF, 0x80);
            QCOMPARE(payload[1], uint8_t(0x80));
            QVERIFY(payload[2] & 0x80);
            QCOMPARE(pictureId(payload), expected);
        }
    }
}

void TestVideoSend::h264UnitsComeBackWhole_data()
{
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("packets");

    QTest::newRow("one byte") << 1 << 1;
    QTest::newRow("exactly one packet") << MAX_PAYLOAD << 1;
    QTest::newRow("one byte over") << MAX_PAYLOAD + 1 << 2;
    QTest::newRow("several packets") << 6000 << 6;
    QTest::newRow("a large picture") << 250000 << (250000 - 1 + MAX_PAYLOAD - FU_HEADER - 1) / (MAX_PAYLOAD - FU_HEADER);
}

void TestVideoSend::h264UnitsComeBackWhole()
{
    QFETCH(int, size);
    QFETCH(int, packets);

    Bytes frame;
    appendNal(frame, NAL_SLICE, static_cast<size_t>(size), 30);

    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, false, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), packets);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    if (packets == 1) {
        QCOMPARE(payloads[0], Bytes(frame.begin() + 4, frame.end()));
    } else {
        for (size_t i = 0; i < payloads.size(); i++) {
            QCOMPARE(int(payloads[i][0]), (NAL_SLICE & ~NAL_TYPE_MASK) | FU_A);
            QCOMPARE(payloads[i][1] & NAL_TYPE_MASK, NAL_SLICE & NAL_TYPE_MASK);
            QCOMPARE((payloads[i][1] & 0x80) != 0, i == 0);
            QCOMPARE((payloads[i][1] & 0x40) != 0, i + 1 == payloads.size());
        }
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h264KeyframeAggregatesItsParameterSets()
{
    const Bytes frame = h264Keyframe(5000);
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, true, MAX_PAYLOAD);

    QVERIFY(payloads.size() > 2);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    const Bytes &aggregate = payloads[0];
    QCOMPARE(aggregate[0] & NAL_TYPE_MASK, STAP_A);
    QCOMPARE(aggregate[0] & 0x60, NAL_SPS & 0x60);
    QCOMPARE(aggregate.size(), size_t(1 + 2 + 21 + 2 + 7));
    QCOMPARE((aggregate[1] << 8) | aggregate[2], 21);
    QCOMPARE(int(aggregate[3]), NAL_SPS);
    QCOMPARE((aggregate[24] << 8) | aggregate[25], 7);
    QCOMPARE(int(aggregate[26]), NAL_PPS);
    QVERIFY(startsWithParameterSets(VideoCodec::H264, aggregate));

    for (size_t i = 1; i < payloads.size(); i++) {
        QCOMPARE(payloads[i][0] & NAL_TYPE_MASK, FU_A);
        QCOMPARE(payloads[i][1] & NAL_TYPE_MASK, NAL_IDR & NAL_TYPE_MASK);
    }
    QVERIFY(payloads[1][1] & 0x80);
    QVERIFY(payloads.back()[1] & 0x40);

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h264KeyframeThatFitsIsOneAggregate()
{
    const Bytes frame = h264Keyframe(200);
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads payloads = packetizer.packetize(frame, true, MAX_PAYLOAD);

    QCOMPARE(payloads.size(), size_t(1));
    QCOMPARE(payloads[0][0] & NAL_TYPE_MASK, STAP_A);
    QCOMPARE(payloads[0].size(), size_t(1 + 2 + 21 + 2 + 7 + 2 + 200));
    QVERIFY(startsKeyframe(VideoCodec::H264, payloads[0]));

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloads, reassembled));
    QCOMPARE(reassembled, frame);

    Bytes larger = frame;
    appendNal(larger, NAL_SLICE, MAX_PAYLOAD - 100, 31);
    const Payloads two = packetizer.packetize(larger, true, MAX_PAYLOAD);
    QCOMPARE(two.size(), size_t(2));
    QCOMPARE(two[0], payloads[0]);
    QCOMPARE(int(two[1][0]), NAL_SLICE);
    QVERIFY(reassemble(VideoCodec::H264, two, reassembled));
    QCOMPARE(reassembled, larger);
}

void TestVideoSend::h264ShortStartCodesComeBackLong()
{
    VideoPacketizer packetizer(VideoCodec::H264);
    const Payloads fromShort = packetizer.packetize(h264Keyframe(5000, 3), true, MAX_PAYLOAD);
    const Payloads fromLong = packetizer.packetize(h264Keyframe(5000, 4), true, MAX_PAYLOAD);
    QVERIFY(!fromShort.empty());
    QCOMPARE(fromShort, fromLong);

    const Bytes frame = { 0, 0, 1, NAL_SLICE, 0x80, 0x55, 0, 0, 0, 0, 1, NAL_SLICE, 0x80, 0x66 };
    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, packetizer.packetize(frame, false, MAX_PAYLOAD), reassembled));
    QCOMPARE(reassembled, (Bytes{ 0, 0, 0, 1, NAL_SLICE, 0x80, 0x55, 0, 0, 0, 0, 1, NAL_SLICE, 0x80, 0x66 }));
}

void TestVideoSend::h265UnitsComeBackWhole_data()
{
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("packets");

    QTest::newRow("one byte of payload") << H265_NAL_HEADER + 1 << 1;
    QTest::newRow("exactly one packet") << MAX_PAYLOAD << 1;
    QTest::newRow("one byte over") << MAX_PAYLOAD + 1 << 2;
    QTest::newRow("several packets") << 6000 << 6;
    QTest::newRow("a large picture") << 250000 << (250000 - H265_NAL_HEADER + H265_FU_ROOM - 1) / H265_FU_ROOM;
}

void TestVideoSend::h265UnitsComeBackWhole()
{
    QFETCH(int, size);
    QFETCH(int, packets);

    Bytes frame;
    appendH265Nal(frame, H265_SLICE, static_cast<size_t>(size), 32);

    VideoPacketizer packetizer(VideoCodec::H265);
    const Payloads payloads = packetizer.packetize(frame, false, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), packets);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));
    QVERIFY(!startsKeyframe(VideoCodec::H265, payloads.front()));

    if (packets == 1) {
        QCOMPARE(payloads[0], Bytes(frame.begin() + 4, frame.end()));
    } else {
        const auto [smallest, largest] = std::minmax_element(payloads.begin(), payloads.end(), [](const Bytes &a, const Bytes &b) { return a.size() < b.size(); });
        QVERIFY(largest->size() - smallest->size() <= 1);

        for (size_t i = 0; i < payloads.size(); i++) {
            QCOMPARE(int(payloads[i][0]), H265_FU << 1);
            QCOMPARE(payloads[i][1], H265_SLICE[1]);
            QCOMPARE(payloads[i][2] & 0x3F, H265_SLICE[0] >> 1);
            QCOMPARE((payloads[i][2] & 0x80) != 0, i == 0);
            QCOMPARE((payloads[i][2] & 0x40) != 0, i + 1 == payloads.size());
        }
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h265KeyframeAggregatesItsParameterSets()
{
    const Bytes frame = h265Keyframe(5000);
    VideoPacketizer packetizer(VideoCodec::H265);
    const Payloads payloads = packetizer.packetize(frame, true, MAX_PAYLOAD);

    QCOMPARE(payloads.size(), size_t(1 + (5000 - H265_NAL_HEADER + H265_FU_ROOM - 1) / H265_FU_ROOM));
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    const Bytes &aggregate = payloads[0];
    const auto headerAt = [&aggregate](size_t at) { return H265Header{ aggregate[at], aggregate[at + 1] }; };
    QCOMPARE(int(aggregate[0]), H265_AP << 1);
    QCOMPARE(int(aggregate[1]), 1);
    QCOMPARE(aggregate.size(), size_t(2 + 2 + 24 + 2 + 40 + 2 + 9));
    QCOMPARE((aggregate[2] << 8) | aggregate[3], 24);
    QVERIFY(headerAt(4) == H265_VPS);
    QCOMPARE((aggregate[28] << 8) | aggregate[29], 40);
    QVERIFY(headerAt(30) == H265_SPS);
    QCOMPARE((aggregate[70] << 8) | aggregate[71], 9);
    QVERIFY(headerAt(72) == H265_PPS);
    QVERIFY(startsWithParameterSets(VideoCodec::H265, aggregate));

    for (size_t i = 1; i < payloads.size(); i++) {
        QCOMPARE(int(payloads[i][0]), H265_FU << 1);
        QCOMPARE(payloads[i][1], H265_IDR[1]);
        QCOMPARE(payloads[i][2] & 0x3F, H265_IDR[0] >> 1);
        QCOMPARE((payloads[i][2] & 0x80) != 0, i == 1);
        QCOMPARE((payloads[i][2] & 0x40) != 0, i + 1 == payloads.size());
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::h265KeyframeThatFitsIsOneAggregate()
{
    const Bytes frame = h265Keyframe(200);
    VideoPacketizer packetizer(VideoCodec::H265);
    const Payloads payloads = packetizer.packetize(frame, true, MAX_PAYLOAD);

    QCOMPARE(payloads.size(), size_t(1));
    QCOMPARE(int(payloads[0][0]), H265_AP << 1);
    QCOMPARE(payloads[0].size(), size_t(2 + 2 + 24 + 2 + 40 + 2 + 9 + 2 + 200));
    QVERIFY(startsKeyframe(VideoCodec::H265, payloads[0]));

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, frame);

    Bytes larger = frame;
    appendH265Nal(larger, H265_SLICE, MAX_PAYLOAD - 100, 33);
    const Payloads two = packetizer.packetize(larger, true, MAX_PAYLOAD);
    QCOMPARE(two.size(), size_t(2));
    QCOMPARE(two[0], payloads[0]);
    QVERIFY((H265Header{ two[1][0], two[1][1] }) == H265_SLICE);
    QVERIFY(reassemble(VideoCodec::H265, two, reassembled));
    QCOMPARE(reassembled, larger);

    const Payloads fromShort = packetizer.packetize(h265Keyframe(5000, 3), true, MAX_PAYLOAD);
    QVERIFY(!fromShort.empty());
    QCOMPARE(fromShort, packetizer.packetize(h265Keyframe(5000, 4), true, MAX_PAYLOAD));
}

void TestVideoSend::h265HeadersKeepLayerAndTemporalId()
{
    // layer 33 has a bit in each header byte
    constexpr H265Header SPS_IN_LAYER_33 = { (33 << 1) | 1, (1 << 3) | 1 };
    constexpr H265Header SLICE_IN_LAYER_33 = { (1 << 1) | 1, (1 << 3) | 3 };
    constexpr H265Header SLICE_IN_LAYER_0 = { 1 << 1, 2 };

    VideoPacketizer packetizer(VideoCodec::H265);
    Bytes reassembled;

    Bytes oneLayer;
    appendH265Nal(oneLayer, SPS_IN_LAYER_33, 10, 34);
    appendH265Nal(oneLayer, SLICE_IN_LAYER_33, 20, 35);
    Payloads payloads = packetizer.packetize(oneLayer, false, MAX_PAYLOAD);
    QCOMPARE(payloads.size(), size_t(1));
    QCOMPARE(int(payloads[0][0]), (H265_AP << 1) | 1);
    QCOMPARE(int(payloads[0][1]), (1 << 3) | 1);
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, oneLayer);

    Bytes twoLayers;
    appendH265Nal(twoLayers, SLICE_IN_LAYER_33, 20, 35);
    appendH265Nal(twoLayers, SLICE_IN_LAYER_0, 20, 36);
    payloads = packetizer.packetize(twoLayers, false, MAX_PAYLOAD);
    QCOMPARE(payloads.size(), size_t(1));
    QCOMPARE(int(payloads[0][0]), H265_AP << 1);
    QCOMPARE(int(payloads[0][1]), 2);
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, twoLayers);

    Bytes large;
    appendH265Nal(large, SLICE_IN_LAYER_33, 3000, 37);
    payloads = packetizer.packetize(large, false, MAX_PAYLOAD);
    QCOMPARE(payloads.size(), size_t(3));
    for (const Bytes &payload : payloads) {
        QCOMPARE(int(payload[0]), (H265_FU << 1) | 1);
        QCOMPARE(payload[1], SLICE_IN_LAYER_33[1]);
        QCOMPARE(payload[2] & 0x3F, 1);
    }
    QVERIFY(reassemble(VideoCodec::H265, payloads, reassembled));
    QCOMPARE(reassembled, large);
}

void TestVideoSend::av1ObusComeBackWhole_data()
{
    QTest::addColumn<int>("size");

    QTest::newRow("one byte of payload") << 2;
    QTest::newRow("exactly one packet") << AV1_ROOM;
    QTest::newRow("one byte over") << AV1_ROOM + 1;
    QTest::newRow("several packets") << 6000;
    QTest::newRow("a large picture") << 250000;
}

void TestVideoSend::av1ObusComeBackWhole()
{
    QFETCH(int, size);

    const Obu picture = { OBU_FRAME, static_cast<size_t>(size - 1) };
    const Bytes frame = av1Frame({ { OBU_TEMPORAL_DELIMITER, 0 }, picture });

    VideoPacketizer packetizer(VideoCodec::AV1);
    const Payloads payloads = packetizer.packetize(frame, false, MAX_PAYLOAD);

    QCOMPARE(static_cast<int>(payloads.size()), (size + AV1_ROOM - 1) / AV1_ROOM);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));
    const auto [smallest, largest] = std::minmax_element(payloads.begin(), payloads.end(), [](const Bytes &a, const Bytes &b) { return a.size() < b.size(); });
    QVERIFY(largest->size() - smallest->size() < payloads.size());

    size_t carried = 0;
    for (size_t i = 0; i < payloads.size(); i++) {
        const int continues = (i > 0 ? AV1_CONTINUES_PREVIOUS : 0) | (i + 1 < payloads.size() ? AV1_CONTINUES_IN_NEXT : 0);
        QCOMPARE(int(payloads[i][0]), continues | av1Elements(1));
        carried += payloads[i].size() - 1;
    }
    QCOMPARE(int(payloads[0][1]), OBU_FRAME << 3);
    QCOMPARE(carried, static_cast<size_t>(size));

    QVERIFY(av1ComesBackAs({ picture }, payloads));
}

void TestVideoSend::av1HeaderCountsUpToThreeElements()
{
    const auto payloadOf = [](int aggregation, const std::vector<Obu> &obus, size_t withLength) {
        Bytes payload = { static_cast<uint8_t>(aggregation) };
        for (size_t i = 0; i < obus.size(); i++) {
            const Bytes element = av1Frame({ obus[i] }, LastObu::Unsized);
            if (i < withLength)
                payload.push_back(static_cast<uint8_t>(element.size()));
            payload.insert(payload.end(), element.begin(), element.end());
        }
        return payload;
    };

    VideoPacketizer packetizer(VideoCodec::AV1);

    const std::vector<Obu> two = { { OBU_SEQUENCE_HEADER, 11 }, { OBU_FRAME, 500 } };
    Payloads payloads = packetizer.packetize(av1Frame(two), true, MAX_PAYLOAD);
    QCOMPARE(payloads, Payloads{ payloadOf(av1Elements(2) | AV1_NEW_SEQUENCE, two, 1) });
    QVERIFY(av1ComesBackAs(two, payloads));

    const std::vector<Obu> three = { { OBU_SEQUENCE_HEADER, 3 }, { OBU_FRAME_HEADER, 4 }, { OBU_TILE_GROUP, 5 } };
    payloads = packetizer.packetize(av1Frame(three), true, MAX_PAYLOAD);
    QCOMPARE(payloads, Payloads{ payloadOf(av1Elements(3) | AV1_NEW_SEQUENCE, three, 2) });
    QVERIFY(av1ComesBackAs(three, payloads));

    const std::vector<Obu> four = { { OBU_SEQUENCE_HEADER, 3 }, { OBU_FRAME_HEADER, 4 }, { OBU_TILE_GROUP, 5 }, { OBU_TILE_GROUP, 6 } };
    payloads = packetizer.packetize(av1Frame(four), true, MAX_PAYLOAD);
    QCOMPARE(payloads, Payloads{ payloadOf(AV1_NEW_SEQUENCE, four, 4) });
    QVERIFY(av1ComesBackAs(four, payloads));

    QCOMPARE(packetizer.packetize(av1Frame(two), false, MAX_PAYLOAD), Payloads{ payloadOf(av1Elements(2), two, 1) });
    const std::vector<Obu> pictureAlone = { { OBU_FRAME, 500 } };
    QCOMPARE(packetizer.packetize(av1Frame(pictureAlone), true, MAX_PAYLOAD), Payloads{ payloadOf(av1Elements(1), pictureAlone, 0) });
}

void TestVideoSend::av1FrameAcrossPacketsSaysWhatContinues()
{
    VideoPacketizer packetizer(VideoCodec::AV1);
    const Payloads payloads = packetizer.packetize(av1Frame(AV1_KEYFRAME_SENT), true, MAX_PAYLOAD);
    QCOMPARE(payloads.size(), size_t(4));
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));

    QCOMPARE(int(payloads[0][0]), AV1_CONTINUES_IN_NEXT | av1Elements(2) | AV1_NEW_SEQUENCE);
    QCOMPARE(int(payloads[0][1]), 12);
    QCOMPARE(int(payloads[0][2]), OBU_SEQUENCE_HEADER << 3);
    QCOMPARE(int(payloads[0][14]), OBU_FRAME << 3);
    QCOMPARE(int(payloads[1][0]), AV1_CONTINUES_PREVIOUS | AV1_CONTINUES_IN_NEXT | av1Elements(1));
    QCOMPARE(int(payloads[2][0]), AV1_CONTINUES_PREVIOUS | AV1_CONTINUES_IN_NEXT | av1Elements(1));
    QCOMPARE(int(payloads[3][0]), AV1_CONTINUES_PREVIOUS | av1Elements(1));

    QVERIFY(startsKeyframe(VideoCodec::AV1, payloads[0]));
    QVERIFY(!startsKeyframe(VideoCodec::AV1, payloads[1]));
    QVERIFY(av1ComesBackAs(AV1_KEYFRAME_KEPT, payloads));

    const Payloads notAKeyframe = packetizer.packetize(av1Frame(AV1_KEYFRAME_SENT), false, MAX_PAYLOAD);
    QCOMPARE(notAKeyframe.size(), size_t(4));
    QCOMPARE(int(notAKeyframe[0][0]), AV1_CONTINUES_IN_NEXT | av1Elements(2));
    QVERIFY(!startsKeyframe(VideoCodec::AV1, notAKeyframe[0]));
}

void TestVideoSend::av1ObuSharesThePacketAnotherEndsIn()
{
    const std::vector<Obu> obus = { { OBU_FRAME_HEADER, 1500 }, { OBU_TILE_GROUP, 100 } };
    VideoPacketizer packetizer(VideoCodec::AV1);
    const Payloads payloads = packetizer.packetize(av1Frame(obus), false, MAX_PAYLOAD);

    QCOMPARE(payloads.size(), size_t(2));
    QCOMPARE(payloads[0].size(), payloads[1].size());
    QCOMPARE(int(payloads[0][0]), AV1_CONTINUES_IN_NEXT | av1Elements(1));

    Bytes length;
    appendLeb128(length, 1 + 1500 - (payloads[0].size() - 1));
    QCOMPARE(length.size(), size_t(2));
    QCOMPARE(int(payloads[1][0]), AV1_CONTINUES_PREVIOUS | av1Elements(2));
    QCOMPARE(Bytes(payloads[1].begin() + 1, payloads[1].begin() + 3), length);
    QCOMPARE(int(payloads[1][payloads[1].size() - 101]), OBU_TILE_GROUP << 3);

    QVERIFY(av1ComesBackAs(obus, payloads));
}

void TestVideoSend::av1ObusThatAreNotSentAreLeftOut()
{
    const Obu delimiter = { OBU_TEMPORAL_DELIMITER, 0 };
    const Obu picture = { OBU_FRAME, 50 };
    Bytes alone = { static_cast<uint8_t>(av1Elements(1)) };
    const Bytes element = av1Frame({ picture }, LastObu::Unsized);
    alone.insert(alone.end(), element.begin(), element.end());

    VideoPacketizer packetizer(VideoCodec::AV1);
    const std::vector<std::vector<Obu>> frames = { { delimiter, picture }, { delimiter, picture, { OBU_PADDING, 30 } }, { delimiter, { OBU_TILE_LIST, 20 }, picture }, { picture, delimiter } };
    for (const std::vector<Obu> &obus : frames)
        QCOMPARE(packetizer.packetize(av1Frame(obus), false, MAX_PAYLOAD), Payloads{ alone });

    QVERIFY(packetizer.packetize(av1Frame({ delimiter }), false, MAX_PAYLOAD).empty());
}

void TestVideoSend::av1LastObuMayComeWithoutItsSize()
{
    VideoPacketizer packetizer(VideoCodec::AV1);
    const Payloads fromSized = packetizer.packetize(av1Frame(AV1_KEYFRAME_KEPT), true, MAX_PAYLOAD);
    QVERIFY(!fromSized.empty());
    QCOMPARE(packetizer.packetize(av1Frame(AV1_KEYFRAME_KEPT, LastObu::Unsized), true, MAX_PAYLOAD), fromSized);

    const Bytes paddedSize = { (OBU_FRAME << 3) | 0x02, 0x83, 0x00, 7, 8, 9 };
    QCOMPARE(packetizer.packetize(paddedSize, false, MAX_PAYLOAD), (Payloads{ { static_cast<uint8_t>(av1Elements(1)), OBU_FRAME << 3, 7, 8, 9 } }));
}

void TestVideoSend::framesComeBackWholeAtAnyPacketSize_data()
{
    QTest::addColumn<int>("codec");
    QTest::addColumn<Bytes>("frame");
    QTest::addColumn<Bytes>("assembled");

    Bytes h264 = h264Keyframe(700);
    appendNal(h264, NAL_SLICE, 40, 110);
    appendNal(h264, NAL_SLICE, 9, 111);
    Bytes h265 = h265Keyframe(700);
    appendH265Nal(h265, H265_SLICE, 40, 112);
    appendH265Nal(h265, H265_SLICE, 9, 113);
    const std::vector<Obu> av1 = { { OBU_SEQUENCE_HEADER, 11 }, { OBU_FRAME_HEADER, 20, true }, { OBU_TILE_GROUP, 300, true }, { OBU_TILE_GROUP, 5 }, { OBU_METADATA, 40 }, { OBU_TILE_GROUP, 150 }, { OBU_TILE_GROUP, 1 } };

    QTest::newRow("H264") << int(VideoCodec::H264) << h264 << h264;
    QTest::newRow("H265") << int(VideoCodec::H265) << h265 << h265;
    QTest::newRow("VP8") << int(VideoCodec::VP8) << vp8Frame(true, 900, 114) << vp8Frame(true, 900, 114);
    QTest::newRow("AV1") << int(VideoCodec::AV1) << av1Frame(av1) << av1Frame(av1, LastObu::Unsized);
}

void TestVideoSend::framesComeBackWholeAtAnyPacketSize()
{
    QFETCH(int, codec);
    QFETCH(Bytes, frame);
    QFETCH(Bytes, assembled);
    const VideoCodec videoCodec = static_cast<VideoCodec>(codec);

    VideoPacketizer packetizer(videoCodec);
    QVERIFY(packetizer.packetize(frame, true, VideoPacketizer::MIN_PAYLOAD_SIZE - 1).empty());

    for (int limit = VideoPacketizer::MIN_PAYLOAD_SIZE; limit <= 400; limit++) {
        const Payloads payloads = packetizer.packetize(frame, true, limit);
        const QByteArray atLimit = QByteArray::number(limit);
        QVERIFY2(noneLargerThan(payloads, static_cast<size_t>(limit)), atLimit.constData());

        Bytes reassembled;
        QVERIFY2(reassemble(videoCodec, payloads, reassembled), atLimit.constData());
        QVERIFY2(reassembled == assembled, atLimit.constData());
    }
}

void TestVideoSend::whatCannotBePacketizedGivesNothing()
{
    const auto known = { VideoCodec::H264, VideoCodec::H265, VideoCodec::VP8, VideoCodec::AV1 };
    for (VideoCodec codec : known) {
        QVERIFY(VideoPacketizer::supports(codec));
        QVERIFY(VideoPacketizer(codec).packetize({}, true, MAX_PAYLOAD).empty());
    }
    QVERIFY(!VideoPacketizer::supports(VideoCodec::Unknown));

    const Bytes frame = h264Keyframe(5000);
    QVERIFY(VideoPacketizer(VideoCodec::Unknown).packetize(frame, true, MAX_PAYLOAD).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H264).packetize(frame, true, 3).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H264).packetize(filler(500, 40), true, MAX_PAYLOAD).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H265).packetize(filler(500, 40), true, MAX_PAYLOAD).empty());
    QVERIFY(VideoPacketizer(VideoCodec::H265).packetize(Bytes{ 0, 0, 1, H265_SLICE[0] }, true, MAX_PAYLOAD).empty());

    const Bytes sizedPastTheEnd = { (OBU_FRAME << 3) | 0x02, 5, 1 };
    const Bytes sizeWithoutEnd = { (OBU_FRAME << 3) | 0x02, 0x80 };
    const Bytes extensionMissing = { (OBU_FRAME << 3) | 0x04 };
    for (const Bytes &malformed : { sizedPastTheEnd, sizeWithoutEnd, extensionMissing })
        QVERIFY(VideoPacketizer(VideoCodec::AV1).packetize(malformed, true, MAX_PAYLOAD).empty());
}

void TestVideoSend::encryptedFramesSurvivePacketizing_data()
{
    QTest::addColumn<int>("codec");
    QTest::addColumn<bool>("keyframe");
    QTest::addColumn<Bytes>("frame");
    QTest::addColumn<Bytes>("decrypted");

    Bytes h265Delta;
    appendH265Nal(h265Delta, H265_SLICE, 900, 52);
    const Obu av1Delta = { OBU_FRAME, 900 };

    QTest::newRow("H264 key frame") << int(VideoCodec::H264) << true << h264Keyframe(5000) << h264Keyframe(5000);
    QTest::newRow("H264 key frame, short start codes") << int(VideoCodec::H264) << true << h264Keyframe(5000, 3) << h264Keyframe(5000, 4);
    QTest::newRow("VP8 key frame") << int(VideoCodec::VP8) << true << vp8Frame(true, 5000, 50) << vp8Frame(true, 5000, 50);
    QTest::newRow("VP8 delta frame") << int(VideoCodec::VP8) << false << vp8Frame(false, 900, 51) << vp8Frame(false, 900, 51);
    QTest::newRow("H265 key frame") << int(VideoCodec::H265) << true << h265Keyframe(5000) << h265Keyframe(5000);
    QTest::newRow("H265 key frame, short start codes") << int(VideoCodec::H265) << true << h265Keyframe(5000, 3) << h265Keyframe(5000, 4);
    QTest::newRow("H265 delta frame") << int(VideoCodec::H265) << false << h265Delta << h265Delta;
    QTest::newRow("AV1 key frame") << int(VideoCodec::AV1) << true << av1Frame(AV1_KEYFRAME_SENT) << av1Frame(AV1_KEYFRAME_KEPT);
    QTest::newRow("AV1 delta frame") << int(VideoCodec::AV1) << false << av1Frame({ { OBU_TEMPORAL_DELIMITER, 0 }, av1Delta }) << av1Frame({ av1Delta });
}

void TestVideoSend::encryptedFramesSurvivePacketizing()
{
    QFETCH(int, codec);
    QFETCH(bool, keyframe);
    QFETCH(Bytes, frame);
    QFETCH(Bytes, decrypted);
    const VideoCodec videoCodec = static_cast<VideoCodec>(codec);

    const Bytes encrypted = encryptFrame(daveCodec(videoCodec), SSRC, frame);
    QVERIFY(encrypted.size() > frame.size());

    VideoPacketizer packetizer(videoCodec);
    const Payloads payloads = packetizer.packetize(encrypted, keyframe, MAX_PAYLOAD);
    QVERIFY(noneLargerThan(payloads, MAX_PAYLOAD));
    QCOMPARE(startsKeyframe(videoCodec, payloads.front()), keyframe);

    Bytes reassembled;
    QVERIFY(reassemble(videoCodec, payloads, reassembled));
    QCOMPARE(reassembled, encrypted);

    Bytes plain;
    QVERIFY(decryptFrame(reassembled, plain));
    prepareForDecoder(videoCodec, plain);
    QCOMPARE(plain, decrypted);
}

// documents an out-of-bounds write in vendored libdave (vendor/libdave/cpp/src/encryptor.cpp, v1.1.1).
// Encryptor::Encrypt writes the grown frame and trailer through raw views of encryptedFrame.data()
// without checking its size, while GetMaxCiphertextByteSize promises only frameSize + 75. each small
// H264 slice widens its start code by a byte and adds a range to the trailer, so 20 of them overrun
// where 19 dont. the asserts that would catch it are compiled out under NDEBUG
void TestVideoSend::encryptWritesPastTheBoundItPromises()
{
    constexpr uint8_t Canary = 0xCD;
    constexpr size_t CanaryTail = 128;

    // 3-byte start code, slice header, one byte covering all three exp-Golomb header values, then two
    // encrypted bytes so the next start code opens a fresh clear range: 7 bytes in, 8 out plus a 6-byte
    // range
    const auto manyNalFrame = [](int units) {
        Bytes frame;
        const uint8_t nal[] = { 0, 0, 1, static_cast<uint8_t>(NAL_SLICE), 0xE0, 0x55, 0x66 };
        for (int i = 0; i < units; i++)
            frame.insert(frame.end(), nal, nal + sizeof(nal));
        return frame;
    };

    struct Measured
    {
        size_t frameSize = 0;
        size_t statedMax = 0;
        size_t bytesWritten = 0;
        size_t overrun = 0;
        bool succeeded = false;
    };

    const auto encryptIntoExactlyTheStatedMax = [&](int units) {
        const Bytes frame = manyNalFrame(units);

        auto encryptor = discord::dave::CreateEncryptor();
        encryptor->SetKeyRatchet(std::make_unique<FixedKeyRatchet>());
        encryptor->AssignSsrcToCodec(SSRC, discord::dave::Codec::H264);

        Measured m;
        m.frameSize = frame.size();
        m.statedMax = encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size());

        // canary-filled past the stated maximum, so an overrun lands in our own allocation
        Bytes buffer(m.statedMax + CanaryTail, Canary);
        const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, SSRC, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(buffer.data(), m.statedMax), &m.bytesWritten);
        m.succeeded = result == discord::dave::IEncryptor::Success;

        for (size_t i = buffer.size(); i-- > m.statedMax;) {
            if (buffer[i] != Canary) {
                m.overrun = i + 1 - m.statedMax;
                break;
            }
        }
        return m;
    };

    const Measured over = encryptIntoExactlyTheStatedMax(20);
    const Measured under = encryptIntoExactlyTheStatedMax(19);

    qInfo() << "H264 20-unit frame:" << over.frameSize << "bytes, stated max" << over.statedMax << ", wrote" << over.bytesWritten << ", overran the stated end by" << over.overrun << "byte(s), succeeded" << over.succeeded;
    qInfo() << "H264 19-unit frame:" << under.frameSize << "bytes, stated max" << under.statedMax << ", wrote" << under.bytesWritten << ", overran the stated end by" << under.overrun << "byte(s), succeeded" << under.succeeded;

    QVERIFY(over.overrun >= 1);
    QVERIFY(over.succeeded);
    QCOMPARE(under.overrun, size_t(0));
    QVERIFY(under.succeeded);
}

void TestVideoSend::packetsAreNumberedMarkedAndExtended()
{
    VideoSendStream sender = vp8Sender();
    const Bytes first = vp8Frame(true, 3000, 60);
    const Bytes second = vp8Frame(false, 2000, 61);
    QVERIFY(sender.queueFrame(first, 90000, true, 0));
    QVERIFY(sender.queueFrame(second, 93000, false, 0));

    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(5));

    for (size_t i = 0; i < packets.size(); i++) {
        const Written &packet = packets[i];
        const bool ofFirstFrame = i < 3;

        QCOMPARE(packet.header.ssrc, SSRC);
        QCOMPARE(packet.header.payloadType, PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, ofFirstFrame ? 90000u : 93000u);
        QCOMPARE(packet.header.sequence, static_cast<uint16_t>(FIRST.sequence + i));
        QCOMPARE(packet.transportSequence(), static_cast<uint16_t>(FIRST.transportSequence + i));
        QCOMPARE(packet.header.marker, i == 2 || i == 4);

        QVERIFY(packet.header.extension);
        QCOMPARE(packet.clearSize, VideoSendStream::CLEAR_SIZE);
        QCOMPARE(packet.extensions.size(), size_t(VideoSendStream::EXTENSIONS_SIZE));
        QCOMPARE(packet.extension(TRANSMISSION_OFFSET_EXTENSION), (Bytes{ 0, 0, 0 }));
        QCOMPARE(packet.extension(ABS_SEND_TIME_EXTENSION).size(), size_t(3));
        QCOMPARE(packet.extension(TRANSPORT_SEQUENCE_EXTENSION).size(), size_t(2));
        for (uint8_t other : { 1, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14 })
            QVERIFY(packet.extension(other).empty());
    }

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::VP8, payloadsOf({ packets.begin(), packets.begin() + 3 }), reassembled));
    QCOMPARE(reassembled, first);
    QVERIFY(reassemble(VideoCodec::VP8, payloadsOf({ packets.begin() + 3, packets.end() }), reassembled));
    QCOMPARE(reassembled, second);

    QCOMPARE(sender.stats().frames, quint64(2));
    QCOMPARE(sender.stats().keyframes, quint64(1));
    QCOMPARE(sender.stats().packets, quint64(5));
    QCOMPARE(sender.stats().bytes, quint64(5000 + 5 * VP8_DESCRIPTOR));
}

void TestVideoSend::theSendTimeIsSecondsInFixedPoint()
{
    VideoSendStream sender = vp8Sender();
    const auto sendTimeAt = [&sender](qint64 nowUs) {
        sender.queueFrame(vp8Frame(false, 10, 62), 0, false, nowUs);
        const std::vector<Written> packets = drain(sender, nowUs);
        return packets.size() == 1 ? packets[0].sendTime() : 0xFFFFFFFF;
    };

    // eighteen bits of fraction: 1.5 s is 1.5 * 2^18
    QCOMPARE(sendTimeAt(1500 * MS), 0x060000u);
    QCOMPARE(sendTimeAt(250 * MS), 0x010000u);
    // six bits of whole seconds, so it wraps every 64 s
    QCOMPARE(sendTimeAt(65 * SECOND), 0x040000u);
    QCOMPARE(sendTimeAt(64 * SECOND - 4), 0xFFFFFFu);
}

void TestVideoSend::noPacketIsLargerThanTheLimit_data()
{
    QTest::addColumn<int>("codec");
    QTest::addColumn<Bytes>("frame");
    QTest::addColumn<int>("largest");

    Bytes unit;
    appendNal(unit, NAL_SLICE, MAX_PAYLOAD, 70);
    Bytes unitOver;
    appendNal(unitOver, NAL_SLICE, MAX_PAYLOAD + 1, 71);

    constexpr int FULL = PACKET_OVERHEAD + MAX_PAYLOAD;
    QTest::newRow("VP8, exactly one packet") << int(VideoCodec::VP8) << vp8Frame(true, VP8_ROOM, 72) << FULL;
    QTest::newRow("VP8, one byte over") << int(VideoCodec::VP8) << vp8Frame(true, VP8_ROOM + 1, 73) << PACKET_OVERHEAD + VP8_DESCRIPTOR + VP8_ROOM / 2 + 1;
    QTest::newRow("VP8, many full packets") << int(VideoCodec::VP8) << vp8Frame(true, 172 * VP8_ROOM, 74) << FULL;
    QTest::newRow("H264, exactly one packet") << int(VideoCodec::H264) << unit << FULL;
    QTest::newRow("H264, one byte over") << int(VideoCodec::H264) << unitOver << PACKET_OVERHEAD + FU_HEADER + MAX_PAYLOAD / 2;
    QTest::newRow("H264, key frame of full packets") << int(VideoCodec::H264) << h264Keyframe(172 * (MAX_PAYLOAD - FU_HEADER) + 1) << FULL;

    Bytes h265Unit;
    appendH265Nal(h265Unit, H265_SLICE, MAX_PAYLOAD, 75);
    Bytes h265UnitOver;
    appendH265Nal(h265UnitOver, H265_SLICE, MAX_PAYLOAD + 1, 76);
    QTest::newRow("H265, exactly one packet") << int(VideoCodec::H265) << h265Unit << FULL;
    QTest::newRow("H265, one byte over") << int(VideoCodec::H265) << h265UnitOver << PACKET_OVERHEAD + H265_FU_HEADER + MAX_PAYLOAD / 2;
    QTest::newRow("H265, key frame of full packets") << int(VideoCodec::H265) << h265Keyframe(H265_NAL_HEADER + 172 * H265_FU_ROOM) << FULL;

    const auto av1Picture = [](int size) { return av1Frame({ { OBU_FRAME, static_cast<size_t>(size - 1) } }, LastObu::Unsized); };
    QTest::newRow("AV1, exactly one packet") << int(VideoCodec::AV1) << av1Picture(AV1_ROOM) << FULL;
    QTest::newRow("AV1, one byte over") << int(VideoCodec::AV1) << av1Picture(AV1_ROOM + 1) << PACKET_OVERHEAD + 1 + MAX_PAYLOAD / 2;
    QTest::newRow("AV1, many full packets") << int(VideoCodec::AV1) << av1Picture(172 * AV1_ROOM) << FULL;
}

void TestVideoSend::noPacketIsLargerThanTheLimit()
{
    QFETCH(int, codec);
    QFETCH(Bytes, frame);
    QFETCH(int, largest);

    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(static_cast<VideoCodec>(codec), PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    sender.setBitrate(100000000);
    QVERIFY(sender.queueFrame(frame, 0, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QVERIFY(!packets.empty());

    int largestSeen = 0;
    std::vector<uint16_t> everySequence;
    for (const Written &packet : packets) {
        largestSeen = std::max(largestSeen, packet.size);
        everySequence.push_back(packet.header.sequence);
    }
    QCOMPARE(largestSeen, largest);
    QVERIFY(largestSeen <= VideoSendStream::MAX_PACKET_SIZE);

    sender.retransmit(everySequence, 0);
    const std::vector<Written> repeated = drain(sender, 0);
    QCOMPARE(repeated.size(), packets.size());
    for (size_t i = 0; i < repeated.size(); i++) {
        QCOMPARE(repeated[i].size, packets[i].size + RTX_HEADER_SIZE);
        QVERIFY(repeated[i].size <= VideoSendStream::MAX_PACKET_SIZE);
        QCOMPARE(repeated[i].header.payloadType, RTX_PAYLOAD_TYPE);
        QCOMPARE(repeated[i].header.marker, packets[i].header.marker);
        QCOMPARE(qFromBigEndian<uint16_t>(repeated[i].payload.data()), packets[i].header.sequence);
        QCOMPARE(Bytes(repeated[i].payload.begin() + RTX_HEADER_SIZE, repeated[i].payload.end()), packets[i].payload);
    }

    Bytes reassembled;
    QVERIFY(reassemble(static_cast<VideoCodec>(codec), payloadsOf(packets), reassembled));
    QCOMPARE(reassembled, frame);
}

void TestVideoSend::aFrameLeavesAtThePaceOfItsBitrate_data()
{
    QTest::addColumn<int>("bitrate");
    QTest::addColumn<bool>("screen");

    QTest::newRow("a camera at 1 Mbps") << 1000000 << false;
    QTest::newRow("a camera at 2 Mbps") << 2000000 << false;
    QTest::newRow("a camera at 3.5 Mbps") << 3500000 << false;
    QTest::newRow("a screen at 2 Mbps") << 2000000 << true;
    QTest::newRow("a screen at 3.5 Mbps") << 3500000 << true;
}

void TestVideoSend::aFrameLeavesAtThePaceOfItsBitrate()
{
    QFETCH(int, bitrate);
    QFETCH(bool, screen);
    constexpr int PACKETS = 20;
    // the official client's: 1.1x the bitrate for a camera, 1x for a screen
    const double factor = screen ? 1.0 : 1.1;
    QCOMPARE(screen ? VideoSendStream::SCREEN_PACING_FACTOR : VideoSendStream::CAMERA_PACING_FACTOR, factor);

    VideoSendStream sender = vp8Sender();
    sender.setBitrate(bitrate);
    if (screen)
        sender.setPacingFactor(VideoSendStream::SCREEN_PACING_FACTOR);
    QVERIFY(sender.queueFrame(vp8Frame(true, PACKETS * VP8_ROOM, 80), 0, true, 0));

    const double burst = burstOf(bitrate, factor);
    const double pace = paceOf(bitrate, factor);
    qint64 left = 0;
    qint64 doneAtMs = -1;
    std::vector<Written> packets;
    for (qint64 ms = 0; sender.hasWaiting(); ms += VideoSendStream::PACE_INTERVAL_MS) {
        QVERIFY(ms < 1000);
        const std::vector<Written> share = interval(sender, ms * MS);
        packets.insert(packets.end(), share.begin(), share.end());
        left += bytesIn(share);
        QVERIFY2(left <= burst + pace * ms + VideoSendStream::MAX_PACKET_SIZE, qPrintable(QStringLiteral("%1 bytes out after %2 ms").arg(left).arg(ms)));
        doneAtMs = ms;
    }

    QCOMPARE(packets.size(), size_t(PACKETS));
    const double earnedAtMs = std::max(0.0, (left - burst) / pace);
    QVERIFY2(doneAtMs <= earnedAtMs + VideoSendStream::PACE_INTERVAL_MS, qPrintable(QStringLiteral("out after %1 ms, earned after %2").arg(doneAtMs).arg(earnedAtMs)));
    QVERIFY(doneAtMs < VideoSendStream::MAX_QUEUE_MS);

    for (size_t i = 0; i < packets.size(); i++)
        QCOMPARE(packets[i].header.sequence, static_cast<uint16_t>(FIRST.sequence + i));
    QVERIFY(packets.back().header.marker);
    if (doneAtMs > 0)
        QVERIFY(packets.front().sendTime() != packets.back().sendTime());
}

void TestVideoSend::aFrameWithinTheBurstLeavesAtOnce()
{
    VideoSendStream sender = vp8Sender();
    sender.setBitrate(3500000);
    // 40 ms of 3.85 Mbps is 19 kB: a frame and a half at 30 fps
    QVERIFY(sender.queueFrame(vp8Frame(false, 12 * VP8_ROOM, 81), 0, false, 0));
    QCOMPARE(interval(sender, 0).size(), size_t(12));
    QVERIFY(!sender.hasWaiting());

    QVERIFY(sender.queueFrame(vp8Frame(false, 12 * VP8_ROOM, 82), 3000, false, 5 * MS));
    QVERIFY(interval(sender, 5 * MS).size() < size_t(12));
    QVERIFY(sender.hasWaiting());
}

void TestVideoSend::aBacklogLeavesWithinTheQueueTime()
{
    VideoSendStream sender = vp8Sender();
    sender.setBitrate(2000000);
    QVERIFY(sender.queueFrame(vp8Frame(true, 400 * VP8_ROOM, 81), 0, true, 0));

    qint64 ms = 0;
    size_t packets = 0;
    for (; sender.hasWaiting(); ms += VideoSendStream::PACE_INTERVAL_MS) {
        QVERIFY(ms <= VideoSendStream::MAX_QUEUE_MS);
        packets += interval(sender, ms * MS).size();
    }
    QCOMPARE(packets, size_t(400));
    QVERIFY(ms >= VideoSendStream::MAX_QUEUE_MS / 2);
}

void TestVideoSend::aLostPacketIsSentAgainOnTheRetransmissionStream()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 5 * VP8_ROOM, 90), 4500, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(5));

    const uint16_t lost[] = { packets[4].header.sequence, packets[1].header.sequence, packets[4].header.sequence };
    sender.retransmit(lost, 200 * MS);
    const std::vector<Written> repeated = drain(sender, 250 * MS);
    QCOMPARE(repeated.size(), size_t(2));

    const Written *originals[] = { &packets[4], &packets[1] };
    for (size_t i = 0; i < repeated.size(); i++) {
        const Written &packet = repeated[i];
        const Written &original = *originals[i];

        QCOMPARE(packet.header.ssrc, RTX_SSRC);
        QCOMPARE(packet.header.payloadType, RTX_PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, 4500u);
        QCOMPARE(packet.header.marker, original.header.marker);
        QCOMPARE(packet.header.sequence, static_cast<uint16_t>(FIRST.rtxSequence + i));
        QCOMPARE(packet.transportSequence(), static_cast<uint16_t>(FIRST.transportSequence + 5 + i));
        QVERIFY(packet.sendTime() != original.sendTime());

        QCOMPARE(packet.payload.size(), original.payload.size() + 2);
        QCOMPARE(qFromBigEndian<uint16_t>(packet.payload.data()), original.header.sequence);
        QCOMPARE(Bytes(packet.payload.begin() + 2, packet.payload.end()), original.payload);
    }

    QCOMPARE(sender.stats().nacksReceived, quint64(3));
    QCOMPARE(sender.stats().retransmitted, quint64(2));
    QCOMPARE(sender.stats().packets, quint64(5));
}

void TestVideoSend::retransmissionsGoAheadOfNewPackets()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 2 * VP8_ROOM, 91), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(2));

    QVERIFY(sender.queueFrame(vp8Frame(false, 12 * VP8_ROOM, 92), 3000, false, 10 * MS));
    const uint16_t lost[] = { sent[0].header.sequence };
    sender.retransmit(lost, 10 * MS);

    const std::vector<Written> share = interval(sender, 10 * MS);
    QVERIFY(share.size() >= 2 && share.size() < 13);
    QCOMPARE(share[0].header.ssrc, RTX_SSRC);
    for (size_t i = 1; i < share.size(); i++) {
        QCOMPARE(share[i].header.ssrc, SSRC);
        QCOMPARE(share[i].header.sequence, static_cast<uint16_t>(sent[1].header.sequence + i));
    }
}

void TestVideoSend::whatWasNeverSentIsNotSentAgain()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 3 * VP8_ROOM, 93), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(3));

    const uint16_t unknown[] = { static_cast<uint16_t>(sent[0].header.sequence - 1), static_cast<uint16_t>(sent[2].header.sequence + 1), 12345 };
    sender.retransmit(unknown, MS);
    QVERIFY(!sender.hasWaiting());
    QVERIFY(drain(sender, MS).empty());
}

void TestVideoSend::aPacketWaitingToGoAgainIsNotQueuedTwice()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 3 * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);

    const uint16_t lost[] = { sent[1].header.sequence };
    sender.retransmit(lost, 100 * MS);
    sender.retransmit(lost, 101 * MS);
    QCOMPARE(drain(sender, 102 * MS).size(), size_t(1));
    QCOMPARE(sender.stats().nacksReceived, quint64(2));
    QCOMPARE(sender.stats().retransmitted, quint64(1));
}

void TestVideoSend::aPacketIsNotSentAgainWithinARoundTrip()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 2 * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    tellRoundTrip(sender, 100);
    const qint64 held = (100 + VideoSendStream::ROUND_TRIP_MARGIN_MS) * MS;

    const uint16_t lost[] = { sent[0].header.sequence };
    sender.retransmit(lost, 110 * MS);
    QCOMPARE(drain(sender, 110 * MS).size(), size_t(1));

    sender.retransmit(lost, 110 * MS + held - 1);
    QVERIFY(!sender.hasWaiting());
    sender.retransmit(lost, 110 * MS + held);
    QCOMPARE(drain(sender, 110 * MS + held).size(), size_t(1));

    const uint16_t other[] = { sent[1].header.sequence };
    sender.retransmit(other, 110 * MS + held + 1);
    QCOMPARE(drain(sender, 110 * MS + held + 1).size(), size_t(1));
}

void TestVideoSend::repairsTakeNoMoreThanTheBitrate()
{
    constexpr int BITRATE = 1000000;
    constexpr int PACKETS = 200;
    VideoSendStream sender = vp8Sender();
    sender.setBitrate(BITRATE);
    QVERIFY(sender.queueFrame(vp8Frame(true, PACKETS * VP8_ROOM, 97), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(PACKETS));

    std::vector<uint16_t> everything;
    for (const Written &packet : sent)
        everything.push_back(packet.header.sequence);

    const size_t share = static_cast<size_t>(BITRATE / 8 * VideoSendStream::RETRANSMISSION_WINDOW_MS / 1000 / sent[0].payload.size());
    QVERIFY(share > 10 && share < PACKETS / 2);

    sender.retransmit(everything, 100 * MS);
    const std::vector<Written> first = drain(sender, 100 * MS);
    QCOMPARE(first.size(), share);
    for (size_t i = 0; i < first.size(); i++)
        QCOMPARE(qFromBigEndian<uint16_t>(first[i].payload.data()), everything[i]);

    const std::span<const uint16_t> rest = std::span<const uint16_t>(everything).subspan(share);
    sender.retransmit(rest, 400 * MS);
    QVERIFY(!sender.hasWaiting());

    sender.retransmit(rest, 100 * MS + VideoSendStream::RETRANSMISSION_WINDOW_MS * MS);
    QCOMPARE(drain(sender, 100 * MS + VideoSendStream::RETRANSMISSION_WINDOW_MS * MS).size(), share);

    sender.setBitrate(2 * BITRATE);
    sender.retransmit(std::span<const uint16_t>(everything).subspan(2 * share), 100 * MS + VideoSendStream::RETRANSMISSION_WINDOW_MS * MS + 1);
    const size_t more = drain(sender, 100 * MS + VideoSendStream::RETRANSMISSION_WINDOW_MS * MS + 1).size();
    QVERIFY(more == share || more == share + 1);
}

void TestVideoSend::aSparseHistoryLastsThreeSeconds()
{
    constexpr qint64 KEPT = VideoSendStream::HISTORY_MS * VideoSendStream::SPARSE_HISTORY_FACTOR * MS;
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 2 * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> early = drain(sender, 0);
    QVERIFY(sender.queueFrame(vp8Frame(false, VP8_ROOM, 95), 3000, false, 600 * MS));
    const std::vector<Written> late = drain(sender, 600 * MS);
    QCOMPARE(early.size() + late.size(), size_t(3));

    const uint16_t first[] = { early[0].header.sequence };
    const uint16_t second[] = { early[1].header.sequence };
    const uint16_t third[] = { late[0].header.sequence };

    sender.retransmit(first, KEPT);
    QCOMPARE(drain(sender, KEPT).size(), size_t(1));

    sender.retransmit(second, KEPT + 1);
    QVERIFY(!sender.hasWaiting());

    sender.retransmit(third, 600 * MS + KEPT);
    const std::vector<Written> repeated = drain(sender, 600 * MS + KEPT);
    QCOMPARE(repeated.size(), size_t(1));
    QCOMPARE(qFromBigEndian<uint16_t>(repeated[0].payload.data()), late[0].header.sequence);
}

void TestVideoSend::aFullHistoryEndsAfterASecond()
{
    constexpr int EXTRA = 10;
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, (VideoSendStream::HISTORY_PACKETS + EXTRA) * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    QCOMPARE(sent.size(), size_t(VideoSendStream::HISTORY_PACKETS + EXTRA));

    const uint16_t first[] = { sent[0].header.sequence };
    sender.retransmit(first, VideoSendStream::HISTORY_MS * MS);
    QCOMPARE(drain(sender, VideoSendStream::HISTORY_MS * MS).size(), size_t(1));

    const uint16_t lastGone[] = { sent[EXTRA].header.sequence };
    const uint16_t firstKept[] = { sent[EXTRA + 1].header.sequence };
    sender.retransmit(lastGone, VideoSendStream::HISTORY_MS * MS + 1);
    QVERIFY(!sender.hasWaiting());
    sender.retransmit(firstKept, VideoSendStream::HISTORY_MS * MS + 1);
    QCOMPARE(drain(sender, VideoSendStream::HISTORY_MS * MS + 1).size(), size_t(1));
}

void TestVideoSend::theHistoryOutlastsThreeRoundTrips()
{
    constexpr int ROUND_TRIP_MS = 600;
    constexpr qint64 KEPT = VideoSendStream::HISTORY_ROUND_TRIPS * (ROUND_TRIP_MS + VideoSendStream::ROUND_TRIP_MARGIN_MS) * MS;
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, (VideoSendStream::HISTORY_PACKETS + 10) * VP8_ROOM, 94), 0, true, 0));
    const std::vector<Written> sent = drain(sender, 0);
    tellRoundTrip(sender, ROUND_TRIP_MS);
    QVERIFY(KEPT > VideoSendStream::HISTORY_MS * MS);

    const uint16_t first[] = { sent[0].header.sequence };
    const uint16_t second[] = { sent[1].header.sequence };
    sender.retransmit(first, KEPT);
    QCOMPARE(drain(sender, KEPT).size(), size_t(1));
    sender.retransmit(second, KEPT + 1);
    QVERIFY(!sender.hasWaiting());
}

void TestVideoSend::withoutARetransmissionPayloadTypeNothingIsSentAgain()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    sender.setCodec(VideoCodec::VP8, PAYLOAD_TYPE, std::nullopt);

    QVERIFY(sender.queueFrame(vp8Frame(true, VP8_ROOM + 2, 96), 0, true, 0));
    const std::vector<Written> packets = drain(sender, 0);
    QCOMPARE(packets.size(), size_t(1));
    QCOMPARE(packets[0].size, VideoSendStream::MAX_PACKET_SIZE);

    const uint16_t lost[] = { packets[0].header.sequence };
    sender.retransmit(lost, MS);
    QVERIFY(!sender.hasWaiting());
}

void TestVideoSend::changingTheCodecKeepsTheNumbering()
{
    VideoSendStream sender = vp8Sender();
    QCOMPARE(sender.codec(), VideoCodec::VP8);
    QVERIFY(sender.queueFrame(vp8Frame(true, 100, 100), 0, true, 0));
    const std::vector<Written> before = drain(sender, 0);
    QCOMPARE(before.size(), size_t(1));

    QVERIFY(sender.queueFrame(vp8Frame(false, 100, 101), 3000, false, 0));
    constexpr uint8_t H264_PAYLOAD_TYPE = 103;
    constexpr uint8_t H264_RTX_PAYLOAD_TYPE = 104;
    sender.setCodec(VideoCodec::H264, H264_PAYLOAD_TYPE, H264_RTX_PAYLOAD_TYPE);
    QCOMPARE(sender.codec(), VideoCodec::H264);
    QVERIFY(!sender.hasWaiting());

    const Bytes frame = h264Keyframe(100);
    QVERIFY(sender.queueFrame(frame, 6000, true, 0));
    const std::vector<Written> after = drain(sender, 0);
    QCOMPARE(after.size(), size_t(1));
    QCOMPARE(after[0].header.payloadType, H264_PAYLOAD_TYPE);
    QCOMPARE(after[0].header.sequence, static_cast<uint16_t>(before[0].header.sequence + 1));
    QCOMPARE(after[0].transportSequence(), static_cast<uint16_t>(before[0].transportSequence() + 1));

    Bytes reassembled;
    QVERIFY(reassemble(VideoCodec::H264, payloadsOf(after), reassembled));
    QCOMPARE(reassembled, frame);

    const uint16_t old[] = { before[0].header.sequence };
    sender.retransmit(old, 0);
    QVERIFY(!sender.hasWaiting());

    QVERIFY(sender.queueFrame(frame, 9000, true, 0));
    sender.setCodec(VideoCodec::H264, H264_PAYLOAD_TYPE, H264_RTX_PAYLOAD_TYPE);
    QVERIFY(sender.hasWaiting());
}

void TestVideoSend::anyCodecCanFollowAnyOther()
{
    struct Sent
    {
        VideoCodec codec;
        uint8_t payloadType;
        Bytes keyframe;
    };
    const Sent codecs[] = {
        { VideoCodec::H264, 103, h264Keyframe(3000) },
        { VideoCodec::H265, 105, h265Keyframe(3000) },
        { VideoCodec::VP8, 101, vp8Frame(true, 3000, 120) },
        { VideoCodec::AV1, 107, av1Frame(AV1_KEYFRAME_KEPT, LastObu::Unsized) },
    };

    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    uint16_t sequence = FIRST.sequence;
    for (const Sent &from : codecs) {
        for (const Sent &to : codecs) {
            if (from.codec == to.codec)
                continue;

            for (const Sent *sent : { &from, &to }) {
                sender.setCodec(sent->codec, sent->payloadType, static_cast<uint8_t>(sent->payloadType + 1));
                QCOMPARE(sender.codec(), sent->codec);
                QVERIFY(sender.queueFrame(sent->keyframe, 0, true, 0));

                const std::vector<Written> packets = drain(sender, 0);
                QVERIFY(!packets.empty());
                QVERIFY(packets.back().header.marker);
                for (const Written &packet : packets) {
                    QCOMPARE(packet.header.payloadType, sent->payloadType);
                    QCOMPARE(packet.header.sequence, sequence);
                    sequence++;
                }

                Bytes reassembled;
                QVERIFY(reassemble(sent->codec, payloadsOf(packets), reassembled));
                QCOMPARE(reassembled, sent->keyframe);
            }
        }
    }
}

void TestVideoSend::clearingForgetsWhatWaited()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(sender.queueFrame(vp8Frame(true, 20 * VP8_ROOM, 102), 0, true, 0));
    const std::vector<Written> sent = interval(sender, 0);
    QVERIFY(!sent.empty() && sent.size() < 20);

    sender.clear();
    QVERIFY(!sender.hasWaiting());
    QVERIFY(interval(sender, 0).empty());
    const uint16_t lost[] = { sent[0].header.sequence };
    sender.retransmit(lost, 0);
    QVERIFY(!sender.hasWaiting());

    QVERIFY(sender.queueFrame(vp8Frame(true, 10, 103), 3000, true, 0));
    const std::vector<Written> next = drain(sender, 0);
    QCOMPARE(next.size(), size_t(1));
    QCOMPARE(next[0].header.sequence, static_cast<uint16_t>(sent.back().header.sequence + 1));
}

void TestVideoSend::framesAreRefusedWithoutACodec()
{
    VideoSendStream sender(SSRC, RTX_SSRC, FIRST);
    QCOMPARE(sender.codec(), VideoCodec::Unknown);
    QVERIFY(!sender.queueFrame(vp8Frame(true, 100, 104), 0, true, 0));

    sender.setCodec(VideoCodec::Unknown, PAYLOAD_TYPE, RTX_PAYLOAD_TYPE);
    QCOMPARE(sender.codec(), VideoCodec::Unknown);
    QVERIFY(!sender.queueFrame(vp8Frame(true, 100, 104), 0, true, 0));
    QVERIFY(!sender.hasWaiting());
    QVERIFY(!sender.canReport());
    QCOMPARE(sender.stats().frames, quint64(0));
}

void TestVideoSend::theSenderReportStatesWhatWasSent()
{
    VideoSendStream sender = vp8Sender();
    uint8_t buffer[64];
    QVERIFY(!sender.canReport());
    QVERIFY(sender.writeSenderReport(buffer, 0, 0) < 0);

    QVERIFY(sender.queueFrame(vp8Frame(true, 3000, 110), 90000, true, 1 * SECOND));
    const std::vector<Written> packets = drain(sender, 1 * SECOND);
    const uint16_t lost[] = { packets[0].header.sequence };
    sender.retransmit(lost, 1 * SECOND);
    QCOMPARE(drain(sender, 1 * SECOND).size(), size_t(1));

    // 2026-01-01 00:00:00.75 UTC in seconds since 1900
    constexpr uint64_t NTP_TIME = (uint64_t(3976214400) << 32) | 0xC0000000u;
    const int size = sender.writeSenderReport(buffer, 1500 * MS, NTP_TIME);
    QCOMPARE(size, 28);
    QCOMPARE(classifyPacket({ buffer, size_t(size) }), PacketKind::Rtcp);

    const ReceivedRtcp parsed = parseRtcp({ buffer, size_t(size) });
    QCOMPARE(parsed.senderReports.size(), size_t(1));
    QVERIFY(parsed.receptionReports.empty());
    const ReceivedRtcp::SenderReport &report = parsed.senderReports[0];
    QCOMPARE(report.ssrc, SSRC);
    QCOMPARE(report.compactNtp, uint32_t((3976214400u << 16) | 0xC000));
    QCOMPARE(report.rtpTimestamp, 90000u + 45000u);
    QCOMPARE(report.packetCount, 3u);
    QCOMPARE(report.octetCount, uint32_t(3000 + 3 * VP8_DESCRIPTOR));
    QCOMPARE(qFromBigEndian<quint64>(buffer + 8), NTP_TIME);

    uint8_t tooSmall[27];
    QVERIFY(sender.writeSenderReport(tooSmall, 1500 * MS, NTP_TIME) < 0);
}

void TestVideoSend::aReceptionReportGivesLossAndRoundTrip()
{
    VideoSendStream sender = vp8Sender();
    QVERIFY(!sender.stats().lossFraction);
    QVERIFY(!sender.stats().roundTripMs);

    QVERIFY(sender.queueFrame(vp8Frame(true, 100, 111), 0, true, 0));
    uint8_t buffer[64];
    constexpr uint64_t FIRST_NTP = (uint64_t(3976214400) << 32);
    constexpr uint64_t SECOND_NTP = (uint64_t(3976214401) << 32);
    QVERIFY(sender.writeSenderReport(buffer, 1 * SECOND, FIRST_NTP) > 0);
    QVERIFY(sender.writeSenderReport(buffer, 2 * SECOND, SECOND_NTP) > 0);

    ReceptionReport report;
    report.ssrc = SSRC;
    report.fractionLost = 64;
    sender.onReceptionReport(report, 2100 * MS);
    QCOMPARE(sender.stats().lossFraction, std::optional<double>(0.25));
    QVERIFY(!sender.stats().roundTripMs);

    report.fractionLost = 0;
    report.lastSenderReport = compactNtp(FIRST_NTP);
    report.delaySinceLastSenderReport = 65536 / 4;
    sender.onReceptionReport(report, 1400 * MS);
    QCOMPARE(sender.stats().lossFraction, std::optional<double>(0.0));
    QCOMPARE(sender.stats().roundTripMs, std::optional<int>(150));

    report.lastSenderReport = compactNtp(SECOND_NTP);
    report.delaySinceLastSenderReport = 0;
    sender.onReceptionReport(report, 2030 * MS);
    QCOMPARE(sender.stats().roundTripMs, std::optional<int>(30));

    report.lastSenderReport = 0x12345678;
    sender.onReceptionReport(report, 5 * SECOND);
    QCOMPARE(sender.stats().roundTripMs, std::optional<int>(30));
}

void TestVideoSend::feedbackForTheSenderIsRead()
{
    constexpr uint32_t SERVER = 0xAABBCCDD;
    constexpr uint32_t OTHER = 0x4321;

    Bytes block;
    append32(block, SERVER);
    append32(block, SSRC);
    block.push_back(32);
    block.insert(block.end(), { 0, 0, 9 });
    append32(block, 70000);
    append32(block, 11);
    append32(block, 0x11223344);
    append32(block, 6554);
    Bytes compound = rtcpPacket(1, 201, block);

    // packet 0xFFFE and, by the bitmask, the first and third after it
    Bytes nack;
    append32(nack, SERVER);
    append32(nack, SSRC);
    append16(nack, 0xFFFE);
    append16(nack, 0x0005);
    append16(nack, 300);
    append16(nack, 0);
    const Bytes nackPacket = rtcpPacket(1, 205, nack);
    compound.insert(compound.end(), nackPacket.begin(), nackPacket.end());

    Bytes transportWide;
    append32(transportWide, SERVER);
    append32(transportWide, SSRC);
    append16(transportWide, 1);
    append16(transportWide, 1);
    append32(transportWide, 0x00000100);
    append16(transportWide, 0x2001);
    append16(transportWide, 0x0400);
    const Bytes transportWidePacket = rtcpPacket(15, 205, transportWide);
    compound.insert(compound.end(), transportWidePacket.begin(), transportWidePacket.end());

    Bytes pictureLoss;
    append32(pictureLoss, SERVER);
    append32(pictureLoss, SSRC);
    const Bytes pictureLossPacket = rtcpPacket(1, 206, pictureLoss);
    compound.insert(compound.end(), pictureLossPacket.begin(), pictureLossPacket.end());

    Bytes fullIntra;
    append32(fullIntra, SERVER);
    append32(fullIntra, 0);
    append32(fullIntra, OTHER);
    fullIntra.insert(fullIntra.end(), { 7, 0, 0, 0 });
    append32(fullIntra, SSRC);
    fullIntra.insert(fullIntra.end(), { 8, 0, 0, 0 });
    const Bytes fullIntraPacket = rtcpPacket(4, 206, fullIntra);
    compound.insert(compound.end(), fullIntraPacket.begin(), fullIntraPacket.end());

    const ReceivedRtcp parsed = parseRtcp(compound);

    QCOMPARE(parsed.receptionReports.size(), size_t(1));
    QCOMPARE(parsed.receptionReports[0].ssrc, SSRC);
    QCOMPARE(parsed.receptionReports[0].fractionLost, uint8_t(32));
    QCOMPARE(parsed.receptionReports[0].cumulativeLost, 9);
    QCOMPARE(parsed.receptionReports[0].lastSenderReport, 0x11223344u);
    QCOMPARE(parsed.receptionReports[0].delaySinceLastSenderReport, 6554u);

    QCOMPARE(parsed.nacks.size(), size_t(1));
    QCOMPARE(parsed.nacks[0].mediaSsrc, SSRC);
    QCOMPARE(parsed.nacks[0].sequences, (std::vector<uint16_t>{ 0xFFFE, 0xFFFF, 1, 300 }));

    QCOMPARE(parsed.keyframeRequests, (std::vector<uint32_t>{ SSRC, OTHER, SSRC }));
    QVERIFY(parsed.receiverCounts.empty());
    QVERIFY(parsed.senderReports.empty());
    QVERIFY(!parsed.inboundEstimate);

    uint8_t written[64];
    const uint16_t missing[] = { 10, 11, 40 };
    const int nackSize = writeNack(written, SERVER, OTHER, missing);
    QVERIFY(nackSize > 0);
    const int pliSize = writePli(std::span(written).subspan(static_cast<size_t>(nackSize)), SERVER, OTHER);
    QVERIFY(pliSize > 0);
    const ReceivedRtcp own = parseRtcp({ written, static_cast<size_t>(nackSize + pliSize) });
    QCOMPARE(own.nacks.size(), size_t(1));
    QCOMPARE(own.nacks[0].mediaSsrc, OTHER);
    QCOMPARE(own.nacks[0].sequences, (std::vector<uint16_t>{ 10, 11, 40 }));
    QCOMPARE(own.keyframeRequests, std::vector<uint32_t>{ OTHER });
}

void TestVideoSend::theReceiverCountIsRead()
{
    constexpr uint32_t SERVER = 0xAABBCCDD;
    const auto receiverCount = [](uint32_t ssrc, int receivers) {
        Bytes body;
        append32(body, ssrc);
        body.insert(body.end(), { 0x30, 0x53, 0x49, 0x44 });
        for (int i = 0; i < receivers; i++) {
            append32(body, 0x00112233);
            append32(body, 0x44556600u + static_cast<uint32_t>(i));
            body.insert(body.end(), { 0x12, 0x34, 0x56, 0x00 });
        }
        return rtcpPacket(static_cast<uint8_t>(receivers), 204, body);
    };

    Bytes compound = rtcpPacket(0, 201, { 0xAA, 0xBB, 0xCC, 0xDD });
    for (const Bytes &packet : { receiverCount(SSRC, 3), receiverCount(0x4321, 0) })
        compound.insert(compound.end(), packet.begin(), packet.end());

    Bytes estimate;
    append32(estimate, SERVER);
    estimate.insert(estimate.end(), { 0x31, 0x53, 0x49, 0x44 });
    append32(estimate, 0);
    append32(estimate, 3);
    // 100000 under an exponent of one, and a quarter lost
    estimate.insert(estimate.end(), { 0x05, 0x86, 0xA0, 64 });
    const Bytes estimatePacket = rtcpPacket(0, 204, estimate);
    compound.insert(compound.end(), estimatePacket.begin(), estimatePacket.end());

    const ReceivedRtcp parsed = parseRtcp(compound);
    QCOMPARE(parsed.receiverCounts.size(), size_t(2));
    QCOMPARE(parsed.receiverCounts[0].ssrc, SSRC);
    QCOMPARE(parsed.receiverCounts[0].receivers, 3);
    QCOMPARE(parsed.receiverCounts[1].ssrc, 0x4321u);
    QCOMPARE(parsed.receiverCounts[1].receivers, 0);
    QVERIFY(parsed.inboundEstimate);
    QCOMPARE(parsed.inboundEstimate->bps, quint64(100000 << 1));
    QCOMPARE(parsed.inboundEstimate->lossFraction, 0.25);

    Bytes other;
    append32(other, SSRC);
    other.insert(other.end(), { 'D', 'I', 'S', '0' });
    QVERIFY(parseRtcp(rtcpPacket(5, 204, other)).receiverCounts.empty());
}

void TestVideoSend::theReceiverEstimateIsRead()
{
    constexpr uint32_t SERVER = 0xAABBCCDD;
    const auto estimate = [](uint8_t exponent, uint32_t mantissa, std::initializer_list<uint32_t> ssrcs, int listed = -1) {
        Bytes body;
        append32(body, SERVER);
        append32(body, 0);
        body.insert(body.end(), { 'R', 'E', 'M', 'B' });
        body.push_back(static_cast<uint8_t>(listed < 0 ? static_cast<int>(ssrcs.size()) : listed));
        body.push_back(static_cast<uint8_t>((exponent << 2) | (mantissa >> 16)));
        body.push_back(static_cast<uint8_t>(mantissa >> 8));
        body.push_back(static_cast<uint8_t>(mantissa));
        for (uint32_t ssrc : ssrcs)
            append32(body, ssrc);
        return rtcpPacket(15, 206, body);
    };
    const auto read = [](std::initializer_list<Bytes> packets) {
        Bytes compound = rtcpPacket(0, 201, { 0xAA, 0xBB, 0xCC, 0xDD });
        for (const Bytes &packet : packets)
            compound.insert(compound.end(), packet.begin(), packet.end());
        return parseRtcp(compound);
    };

    QVERIFY(!read({}).receiverEstimateBps);

    // 2,500,000 as WebRTC writes it: the mantissa shifted down until it fits 18 bits
    ReceivedRtcp received = read({ estimate(4, 156250, { SSRC }) });
    QCOMPARE(received.receiverEstimateBps, std::optional<uint64_t>(2500000));

    QCOMPARE(read({ estimate(0, 0x3FFFF, {}) }).receiverEstimateBps, std::optional<uint64_t>(0x3FFFF));
    QCOMPARE(read({ estimate(0, 0, { SSRC, 0x4321 }) }).receiverEstimateBps, std::optional<uint64_t>(0));

    // of several, the largest, as the official client takes them
    QCOMPARE(read({ estimate(1, 1000, { SSRC }), estimate(3, 1000, { SSRC }), estimate(2, 1000, { SSRC }) }).receiverEstimateBps, std::optional<uint64_t>(8000));

    QVERIFY(!read({ estimate(4, 156250, { SSRC }, 2) }).receiverEstimateBps);
    QVERIFY(!read({ estimate(63, 0x3FFFF, { SSRC }) }).receiverEstimateBps);
    QVERIFY(read({ estimate(46, 0x3FFFF, { SSRC }) }).receiverEstimateBps);

    Bytes other = estimate(4, 156250, { SSRC });
    other[12] = 'X';
    QVERIFY(!read({ other }).receiverEstimateBps);
    QVERIFY(!read({ rtcpPacket(1, 206, { 0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0x12, 0x34 }) }).receiverEstimateBps);
}

void TestVideoSend::aVideoResetRequestIsRead()
{
    const auto reset = [](uint32_t ssrc, std::initializer_list<uint8_t> more = {}) {
        Bytes body;
        append32(body, ssrc);
        body.insert(body.end(), { 0x58, 0x53, 0x49, 0x44 });
        body.insert(body.end(), more);
        return rtcpPacket(0, 204, body);
    };

    Bytes compound = rtcpPacket(0, 201, { 0xAA, 0xBB, 0xCC, 0xDD });
    for (const Bytes &packet : { reset(0x1111), reset(0x2222, { 1, 2, 3, 4 }) })
        compound.insert(compound.end(), packet.begin(), packet.end());
    const ReceivedRtcp received = parseRtcp(compound);

    QCOMPARE(received.videoResets, (std::vector<uint32_t>{ 0x1111, 0x2222 }));
    QVERIFY(received.receiverCounts.empty());
    QVERIFY(!received.inboundEstimate);

    Bytes count;
    append32(count, 0x1111);
    count.insert(count.end(), { 0x30, 0x53, 0x49, 0x44 });
    QVERIFY(parseRtcp(rtcpPacket(0, 204, count)).videoResets.empty());
}

QTEST_GUILESS_MAIN(TestVideoSend)
#include "tst_VideoSend.moc"
