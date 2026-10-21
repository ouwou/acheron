#include "Rtcp.hpp"

#include <QtEndian>

#include <algorithm>
#include <optional>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr uint8_t TYPE_SENDER_REPORT = 200;
constexpr uint8_t TYPE_RECEIVER_REPORT = 201;
constexpr uint8_t TYPE_APPLICATION = 204;
constexpr uint8_t TYPE_PAYLOAD_FEEDBACK = 206;
constexpr uint8_t FORMAT_NACK = 1;
constexpr uint8_t FORMAT_PLI = 1;
constexpr uint8_t FORMAT_FIR = 4;
constexpr uint8_t FORMAT_APPLICATION_LAYER = 15;
constexpr uint8_t COUNT_BITS = 0x1F;
constexpr uint8_t VERSION_MASK = 0xC0;
constexpr size_t TRANSPORT_REPORT_HEADER_SIZE = 20;
constexpr uint8_t RECEIVER_ESTIMATE_NAME[4] = { 'R', 'E', 'M', 'B' };
constexpr int RECEIVER_ESTIMATE_SIZE = 20;
constexpr int REPORT_BLOCK_SIZE = 24;
constexpr int MAX_REPORT_BLOCKS = 31;
constexpr int RECEIVER_REPORT_HEADER_SIZE = 8;
constexpr int SENDER_REPORT_READ_SIZE = 16;
constexpr int SENDER_REPORT_SIZE = 28;
constexpr int FEEDBACK_HEADER_SIZE = 12;
// Discord's "DIS1" as its bytes come off the wire: the native module writes it as one 32-bit number
constexpr uint8_t INBOUND_ESTIMATE_NAME[4] = { 0x31, 0x53, 0x49, 0x44 };
constexpr int INBOUND_ESTIMATE_SIZE = 24;
// "DIS0": the stream's SSRC sits where a sender's would, the count in the header's count bits, then per
// receiver a user id (8), a rate (3) and a byte
constexpr uint8_t RECEIVER_COUNT_NAME[4] = { 0x30, 0x53, 0x49, 0x44 };
// "DISX": the SSRC is one of the remote sender's
constexpr uint8_t VIDEO_RESET_NAME[4] = { 0x58, 0x53, 0x49, 0x44 };
constexpr int APPLICATION_HEADER_SIZE = 12;

void readNack(const uint8_t *packet, size_t size, std::vector<ReceivedRtcp::Nack> &nacks)
{
    ReceivedRtcp::Nack nack;
    nack.mediaSsrc = qFromBigEndian<uint32_t>(packet + 8);
    for (size_t at = FEEDBACK_HEADER_SIZE; at + 4 <= size; at += 4) {
        const uint16_t first = qFromBigEndian<uint16_t>(packet + at);
        const uint16_t following = qFromBigEndian<uint16_t>(packet + at + 2);
        nack.sequences.push_back(first);
        for (int bit = 0; bit < 16; bit++) {
            if (following & (1u << bit))
                nack.sequences.push_back(static_cast<uint16_t>(first + bit + 1));
        }
    }
    if (!nack.sequences.empty())
        nacks.push_back(std::move(nack));
}

// RFC 5104, 4.3.1
void readFullIntraRequest(const uint8_t *packet, size_t size, std::vector<uint32_t> &ssrcs)
{
    constexpr size_t ENTRY_SIZE = 8;
    for (size_t at = FEEDBACK_HEADER_SIZE; at + ENTRY_SIZE <= size; at += ENTRY_SIZE)
        ssrcs.push_back(qFromBigEndian<uint32_t>(packet + at));
}

// draft-holmer-rmcat-transport-wide-cc-extensions-01, 3.1
void readTransportReport(const uint8_t *packet, size_t size, std::vector<ReceivedRtcp::TransportReport> &reports)
{
    if (size < TRANSPORT_REPORT_HEADER_SIZE)
        return;
    if (packet[0] & RTCP_PADDING_BIT) {
        const size_t padding = packet[size - 1];
        if (padding == 0 || padding > size - TRANSPORT_REPORT_HEADER_SIZE)
            return;
        size -= padding;
    }

    ReceivedRtcp::TransportReport report;
    report.mediaSsrc = qFromBigEndian<uint32_t>(packet + 8);
    report.baseSequence = qFromBigEndian<uint16_t>(packet + 12);
    const size_t count = qFromBigEndian<uint16_t>(packet + 14);
    report.referenceTime = signExtend24(readBigEndian24(packet + 16));

    std::vector<uint8_t> statuses;
    size_t at = TRANSPORT_REPORT_HEADER_SIZE;
    while (statuses.size() < count) {
        if (at + 2 > size)
            return;
        const uint16_t chunk = qFromBigEndian<uint16_t>(packet + at);
        at += 2;
        if (!(chunk & 0x8000)) {
            statuses.insert(statuses.end(), chunk & 0x1FFF, static_cast<uint8_t>((chunk >> 13) & 3));
        } else if (!(chunk & 0x4000)) {
            for (int bit = 13; bit >= 0; bit--)
                statuses.push_back(static_cast<uint8_t>((chunk >> bit) & 1));
        } else {
            for (int shift = 12; shift >= 0; shift -= 2)
                statuses.push_back(static_cast<uint8_t>((chunk >> shift) & 3));
        }
    }
    statuses.resize(count);

    report.packets.reserve(count);
    for (const uint8_t status : statuses) {
        ReceivedRtcp::TransportReport::Packet reported;
        if (status == static_cast<uint8_t>(FeedbackStatus::SmallDelta)) {
            if (at + 1 > size)
                return;
            reported.received = true;
            reported.delta = packet[at];
            at += 1;
        } else if (status == static_cast<uint8_t>(FeedbackStatus::LargeDelta)) {
            if (at + 2 > size)
                return;
            reported.received = true;
            reported.delta = qFromBigEndian<int16_t>(packet + at);
            at += 2;
        } else if (status != static_cast<uint8_t>(FeedbackStatus::NotReceived)) {
            return;
        }
        report.packets.push_back(reported);
    }
    reports.push_back(std::move(report));
}

void readReportBlocks(const uint8_t *packet, size_t size, size_t blocksAt, std::vector<ReceptionReport> &reports)
{
    const int count = packet[0] & COUNT_BITS;
    for (int i = 0; i < count && blocksAt + REPORT_BLOCK_SIZE <= size; i++, blocksAt += REPORT_BLOCK_SIZE) {
        const uint8_t *p = packet + blocksAt;
        ReceptionReport report;
        report.ssrc = qFromBigEndian<uint32_t>(p);
        report.fractionLost = p[4];
        report.cumulativeLost = signExtend24(readBigEndian24(p + 5));
        report.highestSequence = qFromBigEndian<uint32_t>(p + 8);
        report.jitter = qFromBigEndian<uint32_t>(p + 12);
        report.lastSenderReport = qFromBigEndian<uint32_t>(p + 16);
        report.delaySinceLastSenderReport = qFromBigEndian<uint32_t>(p + 20);
        reports.push_back(report);
    }
}

std::optional<uint64_t> readPackedRate(const uint8_t *p)
{
    const uint64_t mantissa = (static_cast<uint64_t>(p[0] & 0x03) << 16) | (static_cast<uint64_t>(p[1]) << 8) | p[2];
    const int exponent = p[0] >> 2;
    const uint64_t rate = mantissa << exponent;
    if (rate >> exponent != mantissa)
        return std::nullopt;
    return rate;
}

void readReceiverEstimate(const uint8_t *p, size_t size, ReceivedRtcp &received)
{
    if (size < RECEIVER_ESTIMATE_SIZE || size < RECEIVER_ESTIMATE_SIZE + size_t(4) * p[16] || !std::equal(p + 12, p + 16, RECEIVER_ESTIMATE_NAME))
        return;

    const std::optional<uint64_t> rate = readPackedRate(p + 17);
    if (rate)
        received.receiverEstimateBps = std::max(received.receiverEstimateBps.value_or(0), *rate);
}

} // namespace

void writeRtcpHeader(uint8_t *p, uint8_t countOrFormat, uint8_t type, int size)
{
    p[0] = RTCP_VERSION_BITS | countOrFormat;
    p[1] = type;
    qToBigEndian(static_cast<uint16_t>(size / 4 - 1), p + 2);
}

int writeReceiverReport(std::span<uint8_t> out, uint32_t senderSsrc, std::span<const ReceptionReport> reports)
{
    const int count = static_cast<int>(std::min<size_t>(reports.size(), MAX_REPORT_BLOCKS));
    const int size = RECEIVER_REPORT_HEADER_SIZE + count * REPORT_BLOCK_SIZE;
    if (static_cast<int>(out.size()) < size)
        return -1;

    uint8_t *p = out.data();
    writeRtcpHeader(p, static_cast<uint8_t>(count), TYPE_RECEIVER_REPORT, size);
    qToBigEndian(senderSsrc, p + 4);
    p += RECEIVER_REPORT_HEADER_SIZE;

    for (int i = 0; i < count; i++) {
        const ReceptionReport &report = reports[i];
        qToBigEndian(report.ssrc, p);
        p[4] = report.fractionLost;
        writeBigEndian24(static_cast<uint32_t>(std::clamp(report.cumulativeLost, -0x800000, 0x7FFFFF)), p + 5);
        qToBigEndian(report.highestSequence, p + 8);
        qToBigEndian(report.jitter, p + 12);
        qToBigEndian(report.lastSenderReport, p + 16);
        qToBigEndian(report.delaySinceLastSenderReport, p + 20);
        p += REPORT_BLOCK_SIZE;
    }
    return size;
}

int writeSenderReport(std::span<uint8_t> out, const SenderInfo &info)
{
    if (static_cast<int>(out.size()) < SENDER_REPORT_SIZE)
        return -1;

    uint8_t *p = out.data();
    writeRtcpHeader(p, 0, TYPE_SENDER_REPORT, SENDER_REPORT_SIZE);
    qToBigEndian(info.ssrc, p + 4);
    qToBigEndian(static_cast<quint64>(info.ntpTime), p + 8);
    qToBigEndian(info.rtpTimestamp, p + 16);
    qToBigEndian(info.packetCount, p + 20);
    qToBigEndian(info.octetCount, p + 24);
    return SENDER_REPORT_SIZE;
}

ReceivedRtcp parseRtcp(std::span<const uint8_t> compound)
{
    ReceivedRtcp received;
    while (compound.size() >= RTCP_HEADER_SIZE) {
        const uint8_t *p = compound.data();
        const size_t size = (static_cast<size_t>(qFromBigEndian<uint16_t>(p + 2)) + 1) * 4;
        if ((p[0] & VERSION_MASK) != RTCP_VERSION_BITS || size > compound.size())
            break;

        const uint8_t type = p[1];
        const uint8_t countOrFormat = p[0] & COUNT_BITS;
        if (type == TYPE_SENDER_REPORT && size >= SENDER_REPORT_READ_SIZE) {
            ReceivedRtcp::SenderReport report;
            report.ssrc = qFromBigEndian<uint32_t>(p + 4);
            report.compactNtp = compactNtp(qFromBigEndian<uint64_t>(p + 8));
            if (size >= SENDER_REPORT_SIZE) {
                report.rtpTimestamp = qFromBigEndian<uint32_t>(p + 16);
                report.packetCount = qFromBigEndian<uint32_t>(p + 20);
                report.octetCount = qFromBigEndian<uint32_t>(p + 24);
            }
            received.senderReports.push_back(report);
            readReportBlocks(p, size, SENDER_REPORT_SIZE, received.receptionReports);
        } else if (type == TYPE_RECEIVER_REPORT) {
            readReportBlocks(p, size, RECEIVER_REPORT_HEADER_SIZE, received.receptionReports);
        } else if (type == TYPE_APPLICATION && size >= INBOUND_ESTIMATE_SIZE && std::equal(p + 8, p + 12, INBOUND_ESTIMATE_NAME)) {
            if (const std::optional<uint64_t> rate = readPackedRate(p + 20))
                received.inboundEstimate = InboundEstimate{ *rate, p[23] / 256.0 };
        } else if (type == TYPE_APPLICATION && size >= APPLICATION_HEADER_SIZE && std::equal(p + 8, p + 12, RECEIVER_COUNT_NAME)) {
            received.receiverCounts.push_back({ qFromBigEndian<uint32_t>(p + 4), countOrFormat });
        } else if (type == TYPE_APPLICATION && size >= APPLICATION_HEADER_SIZE && std::equal(p + 8, p + 12, VIDEO_RESET_NAME)) {
            received.videoResets.push_back(qFromBigEndian<uint32_t>(p + 4));
        } else if (type == TYPE_PAYLOAD_FEEDBACK && countOrFormat == FORMAT_APPLICATION_LAYER) {
            readReceiverEstimate(p, size, received);
        } else if (type == TYPE_TRANSPORT_FEEDBACK && countOrFormat == FORMAT_NACK && size >= FEEDBACK_HEADER_SIZE) {
            readNack(p, size, received.nacks);
        } else if (type == TYPE_TRANSPORT_FEEDBACK && countOrFormat == FORMAT_TRANSPORT_WIDE) {
            readTransportReport(p, size, received.transportReports);
        } else if (type == TYPE_PAYLOAD_FEEDBACK && countOrFormat == FORMAT_PLI && size >= FEEDBACK_HEADER_SIZE) {
            received.keyframeRequests.push_back(qFromBigEndian<uint32_t>(p + 8));
        } else if (type == TYPE_PAYLOAD_FEEDBACK && countOrFormat == FORMAT_FIR && size >= FEEDBACK_HEADER_SIZE) {
            readFullIntraRequest(p, size, received.keyframeRequests);
        }
        compound = compound.subspan(size);
    }
    return received;
}

int writeNack(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc, std::span<const uint16_t> sequences)
{
    if (sequences.empty() || static_cast<int>(out.size()) < FEEDBACK_HEADER_SIZE)
        return -1;

    uint8_t *p = out.data();
    int size = FEEDBACK_HEADER_SIZE;
    size_t i = 0;
    while (i < sequences.size()) {
        if (static_cast<int>(out.size()) < size + 4)
            return -1;

        const uint16_t first = sequences[i++];
        uint16_t following = 0;
        while (i < sequences.size()) {
            const uint16_t distance = static_cast<uint16_t>(sequences[i] - first);
            if (distance == 0 || distance > 16)
                break;
            following |= static_cast<uint16_t>(1u << (distance - 1));
            i++;
        }
        qToBigEndian(first, p + size);
        qToBigEndian(following, p + size + 2);
        size += 4;
    }

    writeRtcpHeader(p, FORMAT_NACK, TYPE_TRANSPORT_FEEDBACK, size);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    return size;
}

int writePli(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc)
{
    if (static_cast<int>(out.size()) < FEEDBACK_HEADER_SIZE)
        return -1;

    uint8_t *p = out.data();
    writeRtcpHeader(p, FORMAT_PLI, TYPE_PAYLOAD_FEEDBACK, FEEDBACK_HEADER_SIZE);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    return FEEDBACK_HEADER_SIZE;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
