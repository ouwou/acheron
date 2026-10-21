#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

// RFC 3550, 6.4.1
struct ReceptionReport
{
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;
    int32_t cumulativeLost = 0;
    uint32_t highestSequence = 0;
    uint32_t jitter = 0;
    uint32_t lastSenderReport = 0;
    uint32_t delaySinceLastSenderReport = 0;
};

int writeReceiverReport(std::span<uint8_t> out, uint32_t senderSsrc, std::span<const ReceptionReport> reports);

// RFC 4585, 6.2.1. `sequences` ascending, modulo wrap-around
int writeNack(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc, std::span<const uint16_t> sequences);

// RFC 4585, 6.3.1
int writePli(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

// RFC 3550, 6.4.1
struct SenderInfo
{
    uint32_t ssrc = 0;
    uint64_t ntpTime = 0;
    uint32_t rtpTimestamp = 0;
    uint32_t packetCount = 0;
    uint32_t octetCount = 0;
};

int writeSenderReport(std::span<uint8_t> out, const SenderInfo &info);

inline constexpr uint8_t RTCP_VERSION_BITS = 2 << 6;
inline constexpr uint8_t RTCP_PADDING_BIT = 0x20;
inline constexpr int RTCP_HEADER_SIZE = 4;
inline constexpr uint8_t TYPE_TRANSPORT_FEEDBACK = 205;
inline constexpr uint8_t FORMAT_TRANSPORT_WIDE = 15;
inline constexpr int64_t REFERENCE_UNIT_US = 64000;
inline constexpr int64_t DELTA_UNIT_US = 250;

enum class FeedbackStatus : uint8_t {
    NotReceived = 0,
    SmallDelta = 1,
    LargeDelta = 2,
};

// `countOrFormat` may carry RTCP_PADDING_BIT
void writeRtcpHeader(uint8_t *p, uint8_t countOrFormat, uint8_t type, int size);

[[nodiscard]] constexpr uint32_t readBigEndian24(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

constexpr void writeBigEndian24(uint32_t value, uint8_t *p)
{
    p[0] = static_cast<uint8_t>(value >> 16);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value);
}

[[nodiscard]] constexpr int32_t signExtend24(uint32_t value)
{
    return static_cast<int32_t>(value << 8) >> 8;
}

[[nodiscard]] constexpr uint32_t compactNtp(uint64_t ntpTime)
{
    return static_cast<uint32_t>(ntpTime >> 16);
}

[[nodiscard]] constexpr uint32_t toCompactNtpDelay(int64_t microseconds)
{
    return static_cast<uint32_t>(microseconds * 65536 / 1000000);
}

[[nodiscard]] constexpr int64_t fromCompactNtpDelay(uint32_t delay)
{
    return static_cast<int64_t>(delay) * 1000000 / 65536;
}

struct InboundEstimate
{
    uint64_t bps = 0;
    double lossFraction = 0;
};

struct ReceivedRtcp
{
    struct SenderReport
    {
        uint32_t ssrc = 0;
        uint32_t compactNtp = 0;
        uint32_t rtpTimestamp = 0;
        uint32_t packetCount = 0;
        uint32_t octetCount = 0;
    };
    std::vector<SenderReport> senderReports;

    std::vector<ReceptionReport> receptionReports;

    struct Nack
    {
        uint32_t mediaSsrc = 0;
        std::vector<uint16_t> sequences;
    };
    std::vector<Nack> nacks;

    std::vector<uint32_t> keyframeRequests;

    // Discord's own application-defined packet
    struct ReceiverCount
    {
        uint32_t ssrc = 0;
        int receivers = 0;
    };
    std::vector<ReceiverCount> receiverCounts;

    // REMB. of several in one compound packet the largest counts, as in the official client
    std::optional<uint64_t> receiverEstimateBps;

    struct TransportReport
    {
        uint32_t mediaSsrc = 0;
        uint16_t baseSequence = 0;
        int32_t referenceTime = 0;
        struct Packet
        {
            bool received = false;
            int32_t delta = 0;
        };
        std::vector<Packet> packets;
    };
    std::vector<TransportReport> transportReports;

    // Discord's request to receive a sender's video afresh, by one of its SSRCs
    std::vector<uint32_t> videoResets;

    // Discord's estimate; the official client only shows it in its statistics
    std::optional<InboundEstimate> inboundEstimate;
};

[[nodiscard]] ReceivedRtcp parseRtcp(std::span<const uint8_t> compound);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
