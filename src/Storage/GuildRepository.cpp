#include "GuildRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {
GuildRepository::GuildRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void GuildRepository::saveGuild(const Discord::Guild &guild, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare(R"(
		INSERT OR REPLACE INTO guilds
		(id, name, icon, owner_id)
		VALUES (:id, :name, :icon, :owner_id)
    )");

    q.bindValue(":id", static_cast<qint64>(guild.id.get()));
    q.bindValue(":name", guild.name);
    q.bindValue(":icon", guild.icon);
    q.bindValue(":owner_id", static_cast<qint64>(guild.ownerId.get()));

    if (!q.exec()) {
        qCWarning(LogDB) << "GuildRepository: Save failed:" << q.lastError().text();
    }
}
} // namespace Storage
} // namespace Acheron
