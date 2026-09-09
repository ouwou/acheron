#pragma once

#include <QList>
#include <QString>
#include <optional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Proto {

class ProtoReader;

struct GuildFolder
{
    QList<Core::Snowflake> guildIds;
    std::optional<int64_t> id;
    std::optional<QString> name;
    std::optional<uint64_t> color;

    static GuildFolder fromProto(ProtoReader &reader);
};

struct GuildFolders
{
    QList<GuildFolder> folders;
    QList<Core::Snowflake> guildPositions;

    static GuildFolders fromProto(ProtoReader &reader);
};

struct CustomStatus
{
    QString text;
    uint64_t emojiId = 0;
    QString emojiName;
    uint64_t expiresAtMs = 0; // 0 never expires
    uint64_t createdAtMs = 0;

    static CustomStatus fromProto(ProtoReader &reader);
};

struct StatusSettings
{
    std::optional<QString> status;
    std::optional<CustomStatus> customStatus;
    std::optional<bool> showCurrentGame;
    uint64_t statusExpiresAtMs = 0;

    static StatusSettings fromProto(ProtoReader &reader);
    void mergeFrom(const StatusSettings &other);
};

struct PreloadedUserSettings
{
    std::optional<GuildFolders> guildFolders;
    std::optional<StatusSettings> status;

    static PreloadedUserSettings fromProto(ProtoReader &reader);
    static PreloadedUserSettings fromBase64(const QString &base64);

    void mergeFrom(const PreloadedUserSettings &other);
};

} // namespace Proto
} // namespace Acheron
