#pragma once

#include <QtGlobal>

#include <cstdint>
#include <map>
#include <optional>
#include <span>

#include "RtpPacket.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// draft-holmer-rmcat-transport-wide-cc-extensions-01. the server holds video at a low bitrate until it
// gets these
class TransportFeedback
{
public:
    void onPacket(uint16_t transportSequence, qint64 arrivalUs);

    [[nodiscard]] bool hasPending() const { return !arrivals.empty(); }

    int write(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc);

    static constexpr int MAX_STATUSES = 400;
    static constexpr int MAX_SEQUENCE_JUMP = 10000;

private:
    std::map<qint64, qint64> arrivals;
    SequenceUnwrapper sequences;
    // a packet arriving after the report that covered it isnt reported again, as in libwebrtc
    std::optional<qint64> reportedUpTo;
    uint8_t feedbackCount = 0;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
