#include "UI/Call/CallTileModel.hpp"

#include <QtMath>

#include <cmath>

namespace Acheron {
namespace UI {

TilePlan planTiles(const CallFacts &facts)
{
    TilePlan plan;
    plan.videoCount = (facts.ownShareShown ? 1 : 0) + (facts.ownCameraShown ? 1 : 0);
    for (const CallParticipant &participant : facts.participants) {
        if (participant.userId != facts.accountId)
            plan.videoCount += (participant.streaming ? 1 : 0) + (participant.cameraSending && !participant.cameraDisabled ? 1 : 0);
    }

    const bool anyVideo = plan.videoCount > 0;
    if (!anyVideo && facts.collapsedToBar)
        return plan;

    if (facts.ownShareShown)
        plan.tiles.append(TileKey::streamOf(facts.accountId));
    if (facts.ownCameraShown)
        plan.tiles.append(TileKey::personOf(facts.accountId));
    for (const CallParticipant &participant : facts.participants) {
        if (participant.userId == facts.accountId)
            continue;
        if (participant.streaming)
            plan.tiles.append(TileKey::streamOf(participant.userId));
        // A disabled camera is turned back on from the person's entry in the channel list.
        if (participant.cameraSending && participant.cameraDisabled)
            continue;
        if (participant.cameraSending || facts.nonVideoShown || !anyVideo)
            plan.tiles.append(TileKey::personOf(participant.userId));
    }
    return plan;
}

QList<QRect> arrangeTiles(int count, std::optional<int> focused, const QRect &area)
{
    QList<QRect> places;
    if (count <= 0)
        return places;
    places.reserve(count);

    if (focused && *focused >= 0 && *focused < count) {
        const int stripCount = qMin(count - 1, MAX_STRIP_TILES);
        const int stripHeight = stripCount == 0 ? 0 : qBound(54, area.height() / 5, 144);
        const int largeHeight = area.height() - (stripCount == 0 ? 0 : stripHeight + TILE_GAP);
        const int widest = stripCount == 0 ? 0 : (area.width() - TILE_GAP * (stripCount - 1)) / stripCount;
        const int tileWidth = qMin(stripHeight * 16 / 9, widest);
        const int used = tileWidth * stripCount + TILE_GAP * (stripCount - 1);
        int x = area.left() + (area.width() - used) / 2;
        int inStrip = 0;
        for (int i = 0; i < count; i++) {
            if (i == *focused) {
                places.append(QRect(area.left(), area.top(), area.width(), largeHeight));
            } else if (inStrip < stripCount) {
                places.append(QRect(x, area.top() + largeHeight + TILE_GAP, tileWidth, stripHeight));
                x += tileWidth + TILE_GAP;
                inStrip++;
            } else {
                places.append(QRect());
            }
        }
        return places;
    }

    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const int cellWidth = (area.width() - TILE_GAP * (columns - 1)) / columns;
    const int cellHeight = (area.height() - TILE_GAP * (rows - 1)) / rows;
    for (int i = 0; i < count; i++)
        places.append(QRect(area.left() + (i % columns) * (cellWidth + TILE_GAP), area.top() + (i / columns) * (cellHeight + TILE_GAP), cellWidth, cellHeight));
    return places;
}

} // namespace UI
} // namespace Acheron
