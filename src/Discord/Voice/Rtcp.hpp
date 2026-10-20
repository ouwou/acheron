#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

// One report block of a receiver report (RFC 3550, 6.4.1).
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

// Each writer appends one RTCP packet at the start of `out` and returns its size, or -1 when it
// does not fit. A compound packet is these written back to back, receiver report first.
int writeReceiverReport(std::span<uint8_t> out, uint32_t senderSsrc, std::span<const ReceptionReport> reports);

// Generic NACK (RFC 4585, 6.2.1). `sequences` must be in ascending order, modulo wrap-around.
int writeNack(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc, std::span<const uint16_t> sequences);

// Picture loss indication (RFC 4585, 6.3.1): asks the sender for a key frame.
int writePli(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

// What a sender report says of the stream it is about (RFC 3550, 6.4.1).
struct SenderInfo
{
    uint32_t ssrc = 0;
    // Seconds since 1900 as 32.32 fixed point, and the RTP timestamp of that same moment.
    uint64_t ntpTime = 0;
    uint32_t rtpTimestamp = 0;
    uint32_t packetCount = 0;
    uint32_t octetCount = 0;
};

// A sender report with no report blocks.
int writeSenderReport(std::span<uint8_t> out, const SenderInfo &info);

// The middle 32 bits of an NTP time: what a reception report quotes of the sender report it
// answers.
[[nodiscard]] constexpr uint32_t compactNtp(uint64_t ntpTime)
{
    return static_cast<uint32_t>(ntpTime >> 16);
}

// How long a receiver held the sender report it answers, as its reception report states it: in
// units of 1/65536 of a second.
[[nodiscard]] constexpr uint32_t toCompactNtpDelay(int64_t microseconds)
{
    return static_cast<uint32_t>(microseconds * 65536 / 1000000);
}

[[nodiscard]] constexpr int64_t fromCompactNtpDelay(uint32_t delay)
{
    return static_cast<int64_t>(delay) * 1000000 / 65536;
}

// What a receiver takes from the control packets the server sends it.
struct ReceivedRtcp
{
    struct SenderReport
    {
        uint32_t ssrc = 0;
        // The middle 32 bits of the report's NTP time, which a receiver report echoes back
        // so that the sender can work out the round trip.
        uint32_t compactNtp = 0;
        uint32_t rtpTimestamp = 0;
        uint32_t packetCount = 0;
        uint32_t octetCount = 0;
    };
    std::vector<SenderReport> senderReports;

    // What the server says it has received of the streams this client sends.
    std::vector<ReceptionReport> receptionReports;

    // Packets of a stream this client sends that are asked for again (generic NACK).
    struct Nack
    {
        uint32_t mediaSsrc = 0;
        std::vector<uint16_t> sequences;
    };
    std::vector<Nack> nacks;

    // The streams this client sends that a key frame is asked of, by PLI or by FIR.
    std::vector<uint32_t> keyframeRequests;

    // How many are receiving a video stream this client sends. Discord says so in an
    // application-defined packet of its own.
    struct ReceiverCount
    {
        uint32_t ssrc = 0;
        int receivers = 0;
    };
    std::vector<ReceiverCount> receiverCounts;

    // The most the receiving side says the way from this client carries, in bits per second
    // (REMB). WebRTC, and with it the official client, never aims its sending above it. Of
    // several in one compound packet the largest counts, as the official client has it.
    bool hasReceiverEstimate = false;
    uint64_t receiverEstimateBitsPerSecond = 0;

    // Which of the video packets this client numbered have reached the server, and when by
    // its clock (transport-wide congestion control feedback).
    struct TransportReport
    {
        uint32_t mediaSsrc = 0;
        // The number of the first packet reported on; the rest follow it one by one.
        uint16_t baseSequence = 0;
        // When, in units of 64 ms, the arrival times below are counted from. 24 bits, signed.
        int32_t referenceTime = 0;
        struct Packet
        {
            bool received = false;
            // Quarter milliseconds after the packet received before it, or for the first
            // after the reference time. Negative for one that overtook another.
            int32_t delta = 0;
        };
        std::vector<Packet> packets;
    };
    std::vector<TransportReport> transportReports;

    // Senders whose video the server asks this client to receive afresh, each named by one of
    // that sender's SSRCs. Discord asks in an application-defined packet, and the official
    // client then tears down and rebuilds every video receiver it has for that sender.
    std::vector<uint32_t> videoResets;

    // The server's own estimate of what the way to this client carries, in bits per second,
    // and the share of packets it sees lost on it, 0 to 1. Discord sends this in an
    // application-defined packet; the official client only keeps the number for its statistics.
    bool hasInboundEstimate = false;
    uint64_t inboundBitsPerSecond = 0;
    double inboundLoss = 0;
};

// Reads a compound packet. Packets of other kinds are passed over, and reading stops at
// anything malformed.
[[nodiscard]] ReceivedRtcp parseRtcp(std::span<const uint8_t> compound);

} // namespace Voice
} // namespace Discord
} // namespace Acheron
