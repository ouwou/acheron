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

// discord_protos.discord_users.v1.FrecencyUserSettings
struct FrecencyUserSettings
{
    // Versions.data_version, bumped by the server on every change
    std::optional<uint32_t> dataVersion;
    std::optional<EmojiFrecency> emojiFrecency;
    std::optional<EmojiFrecency> emojiReactionFrecency;

    static FrecencyUserSettings fromProto(ProtoReader &reader);

    // 6 and 13 only for PATCH
    QByteArray toProtoPartial() const;
};

} // namespace Proto
} // namespace Acheron
