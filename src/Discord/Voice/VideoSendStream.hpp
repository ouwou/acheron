#pragma once

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "RtpPacket.hpp"
#include "SendSideEstimator.hpp"
#include "VideoPacketizer.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

[[nodiscard]] constexpr qint64 bytesAtRate(qint64 bitsPerSecond, qint64 us)
{
    return bitsPerSecond * us / 8'000'000;
}

[[nodiscard]] constexpr qint64 rateOf(qint64 bytes, qint64 us)
{
    return bytes * 8'000'000 / us;
}

// pacing, history and retransmission budget follow the official client's WebRTC and its numbers, except
// MAX_QUEUE_MS. the owner calls startInterval() every PACE_INTERVAL_MS while anything waits
class VideoSendStream
{
public:
    struct Numbering
    {
        uint16_t sequence = 0;
        uint16_t rtxSequence = 0;
        uint16_t transportSequence = 0;
        uint16_t pictureId = 0;
    };

    struct Stats
    {
        quint64 packets = 0;
        quint64 bytes = 0;
        quint64 frames = 0;
        quint64 keyframes = 0;
        quint64 retransmitted = 0;
        quint64 nacksReceived = 0;
        quint64 bytesWritten = 0;
        std::optional<double> lossFraction;
        std::optional<int> roundTripMs;
    };

    VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first);

    // `estimator` must outlive this
    void numberWith(SendSideEstimator *estimator) { transport = estimator; }

    [[nodiscard]] VideoCodec codec() const { return packetizer ? packetizer->codec() : VideoCodec::Unknown; }

    void setCodec(VideoCodec codec, uint8_t payloadType, std::optional<uint8_t> rtxPayloadType);

    void setBitrate(int bitsPerSecond);
    void setPacingFactor(double factor);

    bool queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, qint64 nowUs);
    void retransmit(std::span<const uint16_t> sequences, qint64 nowUs);
    void clear();

    [[nodiscard]] bool hasWaiting() const { return !queued.empty() || !resends.empty(); }
    void startInterval(qint64 nowUs);
    [[nodiscard]] bool writeNext(qint64 nowUs, PacketBuffer &packet);

    [[nodiscard]] bool canReport() const { return lastFrame.has_value(); }
    int writeSenderReport(std::span<uint8_t> out, qint64 nowUs, uint64_t ntpTime);
    void onReceptionReport(const ReceptionReport &report, qint64 nowUs);

    [[nodiscard]] const Stats &stats() const { return counters; }

    static constexpr int MAX_PACKET_SIZE = 1200;
    static constexpr int PACE_INTERVAL_MS = 5;
    static constexpr int STARTING_BITRATE = 1000000;
    // WebRTC paces a camera at 1.1x its bitrate and screen content at 1x; the official client marks Go
    // Live as screen content
    static constexpr double CAMERA_PACING_FACTOR = 1.1;
    static constexpr double SCREEN_PACING_FACTOR = 1.0;
    static constexpr int BURST_MS = 40;
    static constexpr int MAX_BURST_BYTES = 63000;
    // WebRTC lets its queue reach two seconds, relying on a bandwidth estimate and frame dropper this
    // client lacks
    static constexpr int MAX_QUEUE_MS = 200;
    static constexpr int RETRANSMISSION_WINDOW_MS = 500;
    // a packet is kept for a second or three round trips, whichever is longer, and three times as long
    // while fewer than HISTORY_PACKETS are held
    static constexpr int HISTORY_MS = 1000;
    static constexpr int HISTORY_ROUND_TRIPS = 3;
    static constexpr int HISTORY_PACKETS = 600;
    static constexpr int SPARSE_HISTORY_FACTOR = 3;
    static constexpr int MAX_HISTORY_PACKETS = 9600;
    // WebRTC's margin on the measured round trip
    static constexpr int ROUND_TRIP_MARGIN_MS = 5;

    static constexpr int CLEAR_SIZE = RtpHeader::FIXED_SIZE + RtpHeader::EXTENSION_PREAMBLE_SIZE;
    static constexpr int EXTENSIONS_SIZE = 12;

private:
    struct Packet
    {
        std::vector<uint8_t> payload;
        uint32_t timestamp = 0;
        bool marker = false;
        qint64 queuedUs = 0;
        uint16_t sequence = 0;
        qint64 sentUs = 0;
        bool resendWaiting = false;
        std::optional<qint64> resentUs;
    };

    struct Resent
    {
        qint64 atUs = 0;
        qint64 bytes = 0;
    };

    struct SentReport
    {
        uint32_t compactNtp = 0;
        qint64 sentUs = 0;
    };

    struct QueuedFrame
    {
        uint32_t timestamp = 0;
        qint64 queuedUs = 0;
    };

    enum class Transmission {
        Original,
        Retransmission,
    };

    [[nodiscard]] bool retransmits() const { return retransmissionSsrc != 0 && rtxPayloadType.has_value(); }
    [[nodiscard]] int maxPayloadSize() const;
    [[nodiscard]] Packet *findSent(uint16_t sequence);
    void forgetOld(qint64 nowUs);
    [[nodiscard]] qint64 roundTripUs() const;
    [[nodiscard]] qint64 pacingRate(qint64 nowUs) const;
    [[nodiscard]] bool takeResendShare(qint64 bytes, qint64 nowUs);
    bool write(PacketBuffer &out, const Packet &packet, Transmission transmission, qint64 nowUs);

    uint32_t mediaSsrc;
    uint32_t retransmissionSsrc;
    uint8_t payloadType = 0;
    std::optional<uint8_t> rtxPayloadType;
    std::optional<VideoPacketizer> packetizer;

    uint16_t nextSequence;
    uint16_t nextRtxSequence;
    uint16_t nextTransportSequence;
    SendSideEstimator *transport = nullptr;
    uint16_t firstPictureId;

    std::deque<Packet> queued;
    std::deque<uint16_t> resends;
    std::deque<Packet> history;
    std::deque<Resent> resentLately;
    qint64 resentLatelyBytes = 0;

    int targetBitrate = STARTING_BITRATE;
    double pacingFactor = CAMERA_PACING_FACTOR;
    qint64 queuedBytes = 0;
    qint64 budget = 0;
    std::optional<qint64> budgetedUs;

    std::optional<QueuedFrame> lastFrame;
    std::deque<SentReport> sentReports;

    Stats counters;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
