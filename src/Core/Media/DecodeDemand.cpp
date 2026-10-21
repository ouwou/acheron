#include "Core/Media/DecodeDemand.hpp"

#include <algorithm>

namespace Acheron {
namespace Core {
namespace Media {

bool DecodeDemand::observe(int backlogMs, quint64 queuesDiscarded, qint64 nowMs)
{
    const bool discardedSinceLast = queuesDiscarded != discardsSeen;
    discardsSeen = queuesDiscarded;
    const bool behind = backlogMs >= BEHIND_MS || discardedSinceLast;

    if (!behind) {
        behindSinceMs.reset();
        if (!keepingUpSinceMs)
            keepingUpSinceMs = nowMs;
        if (halvings == 0 || nowMs - *keepingUpSinceMs < raiseAfterMs)
            return false;

        halvings--;
        raisedAtMs = nowMs;
        keepingUpSinceMs = nowMs;
        return true;
    }

    keepingUpSinceMs.reset();
    if (!behindSinceMs)
        behindSinceMs = nowMs;

    const bool senderHasHadTime = !loweredAtMs || nowMs - *loweredAtMs >= SETTLE_MS;
    const bool behindLongEnough = discardedSinceLast || nowMs - *behindSinceMs >= BEHIND_FOR_MS;
    if (halvings == MAX_HALVINGS || !senderHasHadTime || !behindLongEnough)
        return false;

    lower(nowMs);
    return true;
}

void DecodeDemand::lower(qint64 nowMs)
{
    const bool raiseWasAMistake = raisedAtMs && nowMs - *raisedAtMs < RAISE_ON_TRIAL_MS;
    if (raiseWasAMistake)
        raiseAfterMs = std::min(raiseAfterMs * 2, LONGEST_RAISE_AFTER_MS);

    halvings++;
    loweredAtMs = nowMs;
    behindSinceMs = nowMs;
}

int DecodeDemand::pixelsToAskFor(int shownPixels, qint64 sentPixels) const
{
    const qint64 useful = sentPixels > 0 ? std::min<qint64>(shownPixels, sentPixels) : shownPixels;
    return halvings == 0 ? shownPixels : static_cast<int>(useful >> halvings);
}

} // namespace Media
} // namespace Core
} // namespace Acheron
