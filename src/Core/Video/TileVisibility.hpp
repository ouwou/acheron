#pragma once

#include <QHash>
#include <QtGlobal>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {

// as the official client: a tile that leaves the screen keeps its video for a while in case it comes
// straight back, but only the few most recently hidden do
class TileVisibility
{
public:
    void setVisible(Snowflake userId, bool visible, qint64 nowMs);
    void remove(Snowflake userId);

    [[nodiscard]] bool wanted(Snowflake userId, qint64 nowMs) const;

    [[nodiscard]] bool hasPendingExpiry(qint64 nowMs) const;

    static constexpr int HIDDEN_GRACE_MS = 30000;
    static constexpr int HIDDEN_KEPT = 3;

private:
    struct Tile
    {
        bool visible = false;
        qint64 hiddenSinceMs = 0;
    };

    QHash<Snowflake, Tile> tiles;
};

} // namespace Core
} // namespace Acheron
