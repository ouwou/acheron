#pragma once

#include <QtGlobal>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "VideoDepacketizer.hpp"
#include "VideoFrame.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// Turns the RTP packets of one video SSRC into complete frames, in decode order. It holds
// packets that arrive early, names the ones to ask the sender for again, and when a loss cannot
// be repaired it discards everything up to the next key frame and asks for one.
//
// It keeps no clock and no timer of its own: the owner passes the time in and calls poll().
class VideoReceiver
{
public:
    struct Feedback
    {
        std::vector<uint16_t> nacks;
        bool keyframeRequest = false;
    };

    struct Stats
    {
        quint64 packets = 0;
        quint64 bytes = 0;
        quint64 frames = 0;
        quint64 keyframes = 0;
        quint64 retransmitted = 0;
        quint64 duplicates = 0;
        quint64 strays = 0;
        quint64 nacked = 0;
        quint64 keyframeRequests = 0;
        quint64 resyncs = 0;
    };

    enum class Arrival {
        FirstTime,
        Retransmitted,
    };

    VideoReceiver(uint32_t ssrc, VideoCodec codec);

    [[nodiscard]] uint32_t ssrc() const { return mediaSsrc; }
    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    // `payload` holds exactly the RTP payload: header, extensions and padding already removed,
    // and for a retransmission the original sequence number restored.
    void insert(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival = Arrival::FirstTime);
    // A packet of nothing but padding, which a sender may send on the stream itself. It takes
    // a sequence number like any other, and nothing is missing where it was.
    void skip(uint16_t sequence, uint32_t timestamp, qint64 nowMs);

    [[nodiscard]] std::vector<EncodedVideoFrame> takeFrames();
    [[nodiscard]] Feedback poll(qint64 nowMs);

    // What came out could not be used (it failed to decrypt or decode): start over from a key frame.
    void requestKeyframe();

    [[nodiscard]] const Stats &stats() const { return counters; }
    [[nodiscard]] ReceptionReport receptionReport();

    static constexpr int REORDER_GRACE_MS = 10;
    static constexpr int NACK_RETRY_MS = 100;
    static constexpr int NACK_MAX_REQUESTS = 10;
    static constexpr int NACK_GIVE_UP_MS = 1000;
    static constexpr int MAX_NACKS_PER_POLL = 250;
    // As the official client's WebRTC has them: a key frame is asked for again every 200 ms
    // while one is needed, but not of a stream nothing has come from for five seconds.
    static constexpr int KEYFRAME_REQUEST_INTERVAL_MS = 200;
    static constexpr int SILENT_AFTER_MS = 5000;
    static constexpr int MAX_SEQUENCE_JUMP = 3000;
    static constexpr int MAX_HELD_PACKETS = 4000;

private:
    struct Held
    {
        // None for padding.
        PacketPool::Handle payload;
        uint32_t timestamp = 0;
        bool marker = false;
    };

    struct Missing
    {
        qint64 noticedMs = 0;
        qint64 requestedMs = 0;
        int requests = 0;
    };

    [[nodiscard]] qint64 unwrap(uint16_t sequence) const;
    void take(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival);
    void assemble();
    void resync();
    void noteArrival(uint32_t timestamp, qint64 nowMs);

    uint32_t mediaSsrc;
    VideoCodec videoCodec;

    std::map<qint64, Held> held;
    std::map<qint64, Missing> missing;
    std::vector<EncodedVideoFrame> ready;

    // The next sequence number a frame may start at; -1 while waiting for a key frame.
    qint64 nextSequence = -1;
    qint64 highestSequence = -1;
    // The number a packet from far outside the expected range would be followed by, were it
    // the first of a sender's new numbering and not a stray.
    std::optional<uint16_t> sequenceAfterFarPacket;
    bool keyframeWanted = true;
    qint64 keyframeRequestedMs = -1;
    qint64 lastPacketMs = -1;

    Stats counters;

    // Receiver report bookkeeping (RFC 3550, A.3 and A.8).
    qint64 baseSequence = -1;
    quint64 received = 0;
    qint64 expectedAtLastReport = 0;
    quint64 receivedAtLastReport = 0;
    double jitter = 0;
    qint64 lastTransit = 0;
    bool haveTransit = false;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
