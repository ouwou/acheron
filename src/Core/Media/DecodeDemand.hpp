#pragma once

#include <QtGlobal>

#include <optional>

namespace Acheron {
namespace Core {
namespace Media {

// a decoder with time to spare works off a backlog within moments, so a short one means nothing; one
// that stays behind can only be sent less
class DecodeDemand
{
public:
    bool observe(int backlogMs, quint64 queuesDiscarded, qint64 nowMs);

    [[nodiscard]] double pixelShare() const { return 1.0 / (1 << halvings); }
    [[nodiscard]] int pixelsToAskFor(int shownPixels, qint64 sentPixels) const;

    static constexpr int BEHIND_MS = 250;
    static constexpr int BEHIND_FOR_MS = 2000;
    static constexpr int SETTLE_MS = 5000;
    static constexpr int MAX_HALVINGS = 2;
    static constexpr int FIRST_RAISE_AFTER_MS = 60'000;
    static constexpr int LONGEST_RAISE_AFTER_MS = 480'000;
    static constexpr int RAISE_ON_TRIAL_MS = 60'000;

private:
    void lower(qint64 nowMs);

    int halvings = 0;
    quint64 discardsSeen = 0;
    std::optional<qint64> behindSinceMs;
    std::optional<qint64> keepingUpSinceMs;
    std::optional<qint64> loweredAtMs;
    std::optional<qint64> raisedAtMs;
    int raiseAfterMs = FIRST_RAISE_AFTER_MS;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
