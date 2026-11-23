#pragma once

#include <QtGlobal>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Rtcp.hpp"
#include "RtpPacket.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// the server holds video at a low bitrate until it gets these
class TransportFeedback
{
public:
    void onPacket(uint16_t transportSequence, qint64 arrivalUs);

    [[nodiscard]] bool hasPending() const { return !arrivals.empty(); }

    int write(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

    static constexpr int MAX_STATUSES = 400;
    static constexpr int MAX_SEQUENCE_JUMP = 10000;

private:
    struct Arrival
    {
        qint64 sequence = 0;
        qint64 atUs = 0;
    };

    void hold(qint64 sequence, qint64 arrivalUs);

    std::vector<Arrival> arrivals;
    SequenceUnwrapper sequences;
    // a packet arriving after the report that covered it isnt reported again, as in libwebrtc
    std::optional<qint64> reportedUpTo;
    uint8_t feedbackCount = 0;

    std::vector<FeedbackStatus> statuses;
    std::vector<uint8_t> deltas;
    std::vector<uint16_t> chunks;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
