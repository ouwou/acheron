#include "ChannelRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

ChannelRepository::ChannelRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void ChannelRepository::saveChannel(const Discord::Channel &channel, QSqlDatabase &db)
{
    QSqlQuery q(db);

    q.prepare(R"(
		INSERT INTO channels
		(id, type, position, name, guild_id, parent_id)
		VALUES (:id, :type, :position, :name, :guild_id, :parent_id)
    )");

    q.bindValue(":id", static_cast<qint64>(channel.id.get()));
    q.bindValue(":type", static_cast<qint64>(channel.type.get()));
    q.bindValue(":position", static_cast<qint64>(channel.position.get()));
    q.bindValue(":name", channel.name);
    if (channel.guildId.hasValue())
        q.bindValue(":guild_id", static_cast<qint64>(channel.guildId.get()));
    if (channel.parentId.hasValue())
        q.bindValue(":parent_id", static_cast<qint64>(channel.parentId.get()));

    if (!q.exec())
        qCWarning(LogDB) << "ChannelRepository: Save failed:" << q.lastError().text();
}

} // namespace Storage
} // namespace Acheron
