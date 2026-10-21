#include "TileVisibility.hpp"

namespace Acheron {
namespace Core {

void TileVisibility::setVisible(Snowflake userId, bool visible, qint64 nowMs)
{
    auto it = tiles.find(userId);
    if (it == tiles.end()) {
        tiles.insert(userId, Tile{ visible, nowMs });
        return;
    }
    if (it->visible == visible)
        return;
    it->visible = visible;
    if (!visible)
        it->hiddenSinceMs = nowMs;
}

void TileVisibility::remove(Snowflake userId)
{
    tiles.remove(userId);
}

bool TileVisibility::wanted(Snowflake userId, qint64 nowMs) const
{
    auto it = tiles.constFind(userId);
    if (it == tiles.constEnd())
        return false;
    if (it->visible)
        return true;
    if (nowMs - it->hiddenSinceMs >= HIDDEN_GRACE_MS)
        return false;

    int hiddenMoreRecently = 0;
    for (auto other = tiles.constBegin(); other != tiles.constEnd(); ++other) {
        if (other.key() == userId || other->visible)
            continue;
        if (other->hiddenSinceMs > it->hiddenSinceMs || (other->hiddenSinceMs == it->hiddenSinceMs && other.key() > userId))
            hiddenMoreRecently++;
    }
    return hiddenMoreRecently < HIDDEN_KEPT;
}

bool TileVisibility::hasPendingExpiry(qint64 nowMs) const
{
    for (auto it = tiles.constBegin(); it != tiles.constEnd(); ++it) {
        if (!it->visible && nowMs - it->hiddenSinceMs < HIDDEN_GRACE_MS)
            return true;
    }
    return false;
}

} // namespace Core
} // namespace Acheron
