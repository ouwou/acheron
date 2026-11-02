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

// a codec that leaves a decoder no time to spare is better swapped than decoded. drivers that do part of
// the work on the processor pass for hardware (a GTX 750 Ti's H265), so the time taken is all that shows it
class DecodeStrain
{
public:
    // true when the decoder has been busy most of the time for long enough
    bool observe(double decodeMs, qint64 nowMs);

    static constexpr int WINDOW_MS = 5000;
    static constexpr double BUSY_SHARE = 0.7;
    static constexpr int WINDOWS_IN_A_ROW = 3;

private:
    std::optional<qint64> windowStartMs;
    double busyMs = 0.0;
    int strainedWindows = 0;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
