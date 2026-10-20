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

// The one video stream a connection sends, from whole encoded frames to RTP packets ready for
// the transport cipher. It packetizes and numbers them, lets them out at a measured pace, keeps
// what went out for a while so that a lost packet can be sent again on the RTX stream
// (RFC 4588), and counts what the sender report states.
//
// How fast packets leave, how long they are kept and how much may be sent again are the rules
// of the WebRTC that the official client is built on, with its numbers, read out of its native
// module. The one departure is MAX_QUEUE_MS.
//
// Like VideoReceiver it keeps no clock and no timer of its own: the owner passes the time in,
// in microseconds from any origin, and calls startInterval() every PACE_INTERVAL_MS while
// anything is waiting.
class VideoSendStream
{
public:
    // Where each of the stream's counters starts. The transport-wide one numbers every video
    // packet of the connection, retransmissions included.
    struct Numbering
    {
        uint16_t sequence = 0;
        uint16_t rtxSequence = 0;
        uint16_t transportSequence = 0;
        uint16_t pictureId = 0;
    };

    struct Stats
    {
        // First transmissions and the payload bytes in them, as the sender report counts.
        quint64 packets = 0;
        quint64 bytes = 0;
        quint64 frames = 0;
        quint64 keyframes = 0;
        quint64 retransmitted = 0;
        quint64 nacksReceived = 0;
        // Every byte written for the transport: headers and retransmissions too.
        quint64 bytesWritten = 0;
        // From the server's reception reports. Negative until it has reported.
        double lossFraction = -1;
        int roundTripMs = -1;
    };

    VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first);

    // Has the connection number this stream's packets among all it sends, in place of the
    // stream's own count. The estimator has to outlive the stream.
    void numberWith(SendSideEstimator *estimator) { transport = estimator; }

    [[nodiscard]] uint32_t ssrc() const { return mediaSsrc; }
    [[nodiscard]] VideoCodec codec() const { return packetizer ? packetizer->codec() : VideoCodec::Unknown; }

    // Frames are refused until this has named a codec that can be packetized. Without a payload
    // type for retransmissions none are sent. A change of codec forgets what the old one left.
    void setCodec(VideoCodec codec, uint8_t payloadType, std::optional<uint8_t> rtxPayloadType);

    // What the video is being encoded at, in bits per second. Packets leave at the pacing
    // factor times this, and what is sent again may take up no more than this.
    void setBitrate(int bitsPerSecond);
    void setPacingFactor(double factor);

    // One whole frame, end-to-end encrypted already if it is going to be.
    bool queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, qint64 nowUs);
    // Packets the server asks for again. Those still in the history go out ahead of new ones,
    // but not one that is waiting to already, nor one sent again less than a round trip ago,
    // and once they would take more than their share the rest of the list is dropped.
    void retransmit(std::span<const uint16_t> sequences, qint64 nowUs);
    // Forgets what has not gone out yet, and what was kept for sending again.
    void clear();

    [[nodiscard]] bool hasWaiting() const { return !queued.empty() || !resends.empty(); }
    // Allows out what the pace has earned since the last interval, a burst of BURST_MS at
    // most. The pace rises above the bitrate's when a packet would otherwise wait longer than
    // MAX_QUEUE_MS.
    void startInterval(qint64 nowUs);
    // Writes the next packet of this interval's share into `packet`, with nothing yet sealed:
    // the first CLEAR_SIZE bytes are those the transport cipher leaves readable.
    [[nodiscard]] bool writeNext(qint64 nowUs, PacketBuffer &packet);

    // Nothing is reported before the first frame, as there is no RTP time to state.
    [[nodiscard]] bool canReport() const { return framesQueued; }
    // `ntpTime` is the wall clock's reading at `nowUs`, as 32.32 fixed point.
    int writeSenderReport(std::span<uint8_t> out, qint64 nowUs, uint64_t ntpTime);
    void onReceptionReport(const ReceptionReport &report, qint64 nowUs);

    [[nodiscard]] const Stats &stats() const { return counters; }

    // The largest RTP packet, before the transport cipher adds its tag and nonce.
    static constexpr int MAX_PACKET_SIZE = 1200;
    static constexpr int PACE_INTERVAL_MS = 5;
    static constexpr int STARTING_BITRATE = 1000000;
    // WebRTC paces a camera at 1.1 times its bitrate, and what it is told is screen content
    // at the bitrate itself. The official client tells it so of what it shares in a Go Live.
    static constexpr double CAMERA_PACING_FACTOR = 1.1;
    static constexpr double SCREEN_PACING_FACTOR = 1.0;
    static constexpr int BURST_MS = 40;
    static constexpr int MAX_BURST_BYTES = 63000;
    // WebRTC lets its queue grow to two seconds, and has a bandwidth estimate and a frame
    // dropper to keep it from getting there. This client has neither, so an encoder that
    // overshoots its bitrate for long would leave the video that far behind.
    static constexpr int MAX_QUEUE_MS = 200;
    static constexpr int RETRANSMISSION_WINDOW_MS = 500;
    // A packet is kept for a second or three round trips, whichever is longer, and three
    // times as long while fewer than HISTORY_PACKETS are held.
    static constexpr int HISTORY_MS = 1000;
    static constexpr int HISTORY_ROUND_TRIPS = 3;
    static constexpr int HISTORY_PACKETS = 600;
    static constexpr int SPARSE_HISTORY_FACTOR = 3;
    static constexpr int MAX_HISTORY_PACKETS = 9600;
    // What WebRTC adds to the round trip it measured before holding repeats to it.
    static constexpr int ROUND_TRIP_MARGIN_MS = 5;

    static constexpr int CLEAR_SIZE = RtpHeader::FIXED_SIZE + RtpHeader::EXTENSION_PREAMBLE_SIZE;
    // The header extensions every video packet carries, by the ids Discord gives them.
    static constexpr uint8_t TRANSMISSION_OFFSET_EXTENSION = 2;
    static constexpr uint8_t ABS_SEND_TIME_EXTENSION = 3;
    static constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;
    // Those three as one-byte-form elements, padded to whole words.
    static constexpr int EXTENSIONS_SIZE = 12;
    // A retransmission's payload opens with the sequence number the packet first had.
    static constexpr int RTX_HEADER_SIZE = 2;

private:
    struct Packet
    {
        std::vector<uint8_t> payload;
        uint32_t timestamp = 0;
        bool marker = false;
        qint64 queuedUs = 0;
        // Given when it first goes out.
        uint16_t sequence = 0;
        qint64 sentUs = 0;
        bool resendWaiting = false;
        // Negative until it has been sent again.
        qint64 resentUs = -1;
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

    [[nodiscard]] bool retransmits() const { return retransmissionSsrc != 0 && rtxPayloadType.has_value(); }
    [[nodiscard]] int maxPayloadSize() const;
    [[nodiscard]] Packet *findSent(uint16_t sequence);
    void forgetOld(qint64 nowUs);
    [[nodiscard]] qint64 roundTripUs() const;
    [[nodiscard]] qint64 pacingRate(qint64 nowUs) const;
    [[nodiscard]] bool takeResendShare(qint64 bytes, qint64 nowUs);
    bool write(PacketBuffer &out, const Packet &packet, bool retransmission, qint64 nowUs);

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
    // In sequence order with no gaps, oldest first.
    std::deque<Packet> history;
    std::deque<Resent> resentLately;
    qint64 resentLatelyBytes = 0;

    int targetBitrate = STARTING_BITRATE;
    double pacingFactor = CAMERA_PACING_FACTOR;
    qint64 queuedBytes = 0;
    // Bytes that may leave now. A packet may take it below zero, which the next intervals
    // then make up for.
    qint64 budget = 0;
    qint64 budgetedUs = -1;

    bool framesQueued = false;
    uint32_t lastFrameTimestamp = 0;
    qint64 lastFrameQueuedUs = 0;
    std::deque<SentReport> sentReports;

    Stats counters;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
