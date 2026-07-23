#include "GuildRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

#include <QSqlQuery>
#include <QSqlError>

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

    execLogged(q, "GuildRepository: Save");
}

void GuildRepository::deleteGuild(Core::Snowflake guildId, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM guilds WHERE id = :id");
    q.bindValue(":id", static_cast<qint64>(guildId));
    execLogged(q, "GuildRepository: Delete");
}

std::optional<Discord::Guild> GuildRepository::getGuild(Core::Snowflake guildId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, name, icon, owner_id
        FROM guilds WHERE id = :id
    )");
    q.bindValue(":id", static_cast<qint64>(guildId));

    if (!q.exec() || !q.next())
        return std::nullopt;

    Discord::Guild guild;
    guild.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
    guild.name = q.value(1).toString();
    guild.icon = q.value(2).toString();
    guild.ownerId = static_cast<Core::Snowflake>(q.value(3).toLongLong());

    return guild;
}
} // namespace Storage
} // namespace Acheron
