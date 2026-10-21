#pragma once

#include <QList>
#include <QRect>

#include <optional>
#include <tuple>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

struct TileKey
{
    enum class Kind {
        Person,
        Stream,
    };

    Kind kind = Kind::Person;
    Core::Snowflake userId;

    static TileKey personOf(Core::Snowflake userId) { return { Kind::Person, userId }; }
    static TileKey streamOf(Core::Snowflake userId) { return { Kind::Stream, userId }; }
    [[nodiscard]] bool isStream() const { return kind == Kind::Stream; }

    bool operator==(const TileKey &other) const { return kind == other.kind && userId == other.userId; }
    bool operator!=(const TileKey &other) const { return !(*this == other); }
    bool operator<(const TileKey &other) const { return std::tie(kind, userId) < std::tie(other.kind, other.userId); }
};

struct CallParticipant
{
    Core::Snowflake userId;
    bool streaming = false;
    bool cameraSending = false;
    bool cameraDisabled = false;
};

struct CallFacts
{
    Core::Snowflake accountId;
    QList<CallParticipant> participants;
    bool ownShareShown = false;
    bool ownCameraShown = false;
    bool nonVideoShown = true;
    bool collapsedToBar = false;
};

struct TilePlan
{
    QList<TileKey> tiles;
    int videoCount = 0;
};

[[nodiscard]] TilePlan planTiles(const CallFacts &facts);

constexpr int TILE_GAP = 2;
constexpr int MAX_STRIP_TILES = 12;

[[nodiscard]] QList<QRect> arrangeTiles(int count, std::optional<int> focused, const QRect &area);

} // namespace UI
} // namespace Acheron
