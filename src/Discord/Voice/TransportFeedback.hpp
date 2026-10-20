#pragma once

#include <QtGlobal>

#include <cstdint>
#include <map>
#include <span>

namespace Acheron {
namespace Discord {
namespace Voice {

// Builds transport-wide congestion control feedback
// (draft-holmer-rmcat-transport-wide-cc-extensions-01): the report, sent several times a
// second, of which numbered packets arrived and exactly when. The server works out from it how
// much the connection can carry, and sends video at a low bitrate until it has been told.
//
// It keeps no clock: arrival times are passed in, in microseconds.
class TransportFeedback
{
public:
    // A packet carrying the transport-wide sequence number extension has arrived.
    void onPacket(uint16_t transportSequence, qint64 arrivalUs);

    [[nodiscard]] bool hasPending() const { return !arrivals.empty(); }

    // Writes one feedback packet covering what has arrived since the last one, as far as one
    // packet can hold, and forgets what it reported. Returns the size, 0 when there is
    // nothing to report, or -1 when `out` is too small to report anything.
    int write(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

    // The most packets one report describes, received or not.
    static constexpr int MAX_STATUSES = 400;
    // A newer packet this far from everything pending means the numbering restarted.
    static constexpr int MAX_SEQUENCE_JUMP = 10000;

private:
    [[nodiscard]] qint64 unwrap(uint16_t sequence) const;

    // Arrival time of each packet not yet reported, by unwrapped sequence number.
    std::map<qint64, qint64> arrivals;
    qint64 highestSequence = -1;
    // Every packet before this one has been reported, as received or not. One that arrives
    // after its report is not reported again, as libwebrtc does not.
    qint64 reportedUpTo = -1;
    uint8_t feedbackCount = 0;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
