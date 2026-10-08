#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <cstdint>
#include <optional>

namespace Acheron {
namespace Proto {

class ProtoReader;

struct FrecencyItem
{
    uint32_t totalUses = 0;
    QList<uint64_t> recentUses;
    int32_t frecency = 0;
    int32_t score = 0;

    static FrecencyItem fromProto(ProtoReader &reader);
};

struct EmojiFrecency
{
    QHash<QString, FrecencyItem> emojis;

    static EmojiFrecency fromProto(ProtoReader &reader);
};

enum class GifType : uint8_t {
    None = 0,
    Image = 1,
    Video = 2,
};

struct FavoriteGif
{
    QString url;
    GifType format = GifType::None;
    QString src;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t order = 0;
    QByteArray messageAsReceived;
};

struct FavoriteGifs
{
    QList<FavoriteGif> gifs;
    bool hideTooltip = false;
    QByteArray unknownFields;

    static FavoriteGifs fromProto(const QByteArray &bytes);
    [[nodiscard]] QByteArray toProto() const;
};

// discord_protos.discord_users.v1.FrecencyUserSettings
struct FrecencyUserSettings
{
    // Versions.data_version, bumped by the server on every change
    std::optional<uint32_t> dataVersion;
    std::optional<FavoriteGifs> favoriteGifs;
    std::optional<EmojiFrecency> emojiFrecency;
    std::optional<EmojiFrecency> emojiReactionFrecency;

    static FrecencyUserSettings fromProto(ProtoReader &reader);

    QByteArray toProtoPartial() const;
};

} // namespace Proto
} // namespace Acheron
