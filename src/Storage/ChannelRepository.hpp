#pragma once

#include <optional>

#include "BaseRepository.hpp"

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Storage {

class ChannelRepository : public BaseRepository
{
public:
    ChannelRepository(Core::Snowflake accountId);

    void saveChannel(const Discord::Channel &channel, QSqlDatabase &db);
    void deleteChannel(Core::Snowflake channelId, QSqlDatabase &db);
    void savePermissionOverwrites(Core::Snowflake channelId,
                                  const QList<Discord::PermissionOverwrite> &overwrites,
                                  QSqlDatabase &db);
    void saveChannelRecipients(Core::Snowflake channelId,
                               const QList<Core::Snowflake> &recipientIds,
                               QSqlDatabase &db);
    QList<Discord::PermissionOverwrite> getPermissionOverwrites(Core::Snowflake channelId);
    QList<Core::Snowflake> getChannelRecipientIds(Core::Snowflake channelId);

    QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> getPermissionOverwritesForGuild(
            Core::Snowflake guildId);

    std::optional<Discord::Channel> getChannel(Core::Snowflake channelId);
};

} // namespace Storage
} // namespace Acheron
