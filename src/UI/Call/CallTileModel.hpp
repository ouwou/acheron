#pragma once

#include <QList>
#include <QRect>

#include <optional>
#include <tuple>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

// A tile of a call: someone's camera, or their avatar while it is off, or their stream.
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
    // Its camera's video turned off by the viewer, which leaves it no tile at all.
    bool cameraDisabled = false;
};

struct CallFacts
{
    Core::Snowflake accountId;
    // Everyone in the call. The account's own share and camera are told apart below, since
    // they show while they start or after they have failed.
    QList<CallParticipant> participants;
    bool ownShareShown = false;
    bool ownCameraShown = false;
    bool nonVideoShown = true;
    // The view is docked and collapsed to its bar, which over a call without video needs no
    // tiles to draw on.
    bool collapsedToBar = false;
};

struct TilePlan
{
    QList<TileKey> tiles;
    // Streams and cameras that show, the account's own among them.
    int videoCount = 0;
};

[[nodiscard]] TilePlan planTiles(const CallFacts &facts);

constexpr int TILE_GAP = 2;
constexpr int MAX_STRIP_TILES = 12;

// Where each of `count` tiles goes in `area`, in the order they are given. A focused tile fills
// most of it over a strip of the others; those that do not fit in the strip get an empty
// rectangle, and are not shown.
[[nodiscard]] QList<QRect> arrangeTiles(int count, std::optional<int> focused, const QRect &area);

} // namespace UI
} // namespace Acheron
