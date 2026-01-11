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

struct PreloadedUserSettings
{
    std::optional<GuildFolders> guildFolders;

    static PreloadedUserSettings fromProto(ProtoReader &reader);
};

} // namespace Proto
} // namespace Acheron
