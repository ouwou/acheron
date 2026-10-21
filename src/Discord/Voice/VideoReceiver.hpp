#pragma once

#include <QtGlobal>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "RtpPacket.hpp"
#include "VideoDepacketizer.hpp"
#include "VideoFrame.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

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

    [[nodiscard]] VideoCodec codec() const { return videoCodec; }

    void insert(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival = Arrival::FirstTime);
    void skip(uint16_t sequence, uint32_t timestamp, qint64 nowMs);

    [[nodiscard]] std::vector<EncodedVideoFrame> takeFrames();
    [[nodiscard]] Feedback poll(qint64 nowMs);

    void requestKeyframe();

    [[nodiscard]] const Stats &stats() const { return counters; }
    [[nodiscard]] ReceptionReport receptionReport();

    static constexpr int REORDER_GRACE_MS = 10;
    static constexpr int NACK_RETRY_MS = 100;
    static constexpr int NACK_MAX_REQUESTS = 10;
    static constexpr int NACK_GIVE_UP_MS = 1000;
    static constexpr int MAX_NACKS_PER_POLL = 250;
    // as the official client's WebRTC: asked again every 200 ms while needed, but not of a stream silent
    // for five seconds
    static constexpr int KEYFRAME_REQUEST_INTERVAL_MS = 200;
    static constexpr int SILENT_AFTER_MS = 5000;
    static constexpr int MAX_SEQUENCE_JUMP = 3000;
    static constexpr int MAX_HELD_PACKETS = 4000;

private:
    struct Held
    {
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

    void take(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival);
    void assemble();
    void resync();
    void noteArrival(uint32_t timestamp, qint64 nowMs);

    uint32_t mediaSsrc;
    VideoCodec videoCodec;

    std::map<qint64, Held> held;
    std::map<qint64, Missing> missing;
    std::vector<EncodedVideoFrame> ready;

    std::optional<qint64> nextSequence;
    SequenceUnwrapper sequences;
    std::optional<uint16_t> sequenceAfterFarPacket;
    bool keyframeWanted = true;
    std::optional<qint64> keyframeRequestedMs;
    std::optional<qint64> lastPacketMs;

    Stats counters;

    // RFC 3550, A.3
    struct Reception
    {
        std::optional<qint64> baseSequence;
        quint64 received = 0;
        qint64 expectedAtLastReport = 0;
        quint64 receivedAtLastReport = 0;
    };
    Reception reception;

    // RFC 3550, A.8
    double jitter = 0;
    std::optional<qint64> lastTransit;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
