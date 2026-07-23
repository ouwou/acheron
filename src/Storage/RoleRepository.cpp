#include "RoleRepository.hpp"
#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

namespace Acheron {
namespace Storage {

static Discord::Role readRoleFromQuery(const QSqlQuery &q)
{
    Discord::Role role;
    role.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
    role.name = q.value(1).toString();
    role.permissions = Discord::Permissions::fromInt(q.value(2).toLongLong());
    role.position = q.value(3).toInt();
    if (!q.value(4).isNull())
        role.color = q.value(4).toInt();
    if (!q.value(5).isNull())
        role.hoist = q.value(5).toBool();
    if (!q.value(6).isNull())
        role.icon = q.value(6).toString();
    if (!q.value(7).isNull())
        role.unicodeEmoji = q.value(7).toString();
    if (!q.value(8).isNull())
        role.managed = q.value(8).toBool();
    if (!q.value(9).isNull())
        role.mentionable = q.value(9).toBool();
    return role;
}

RoleRepository::RoleRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

static const char *ROLE_UPSERT_SQL = R"(
    INSERT OR REPLACE INTO roles
    (id, guild_id, name, permissions, position, color, hoist, icon,
     unicode_emoji, managed, mentionable)
    VALUES (:id, :guild_id, :name, :permissions, :position, :color, :hoist,
            :icon, :unicode_emoji, :managed, :mentionable)
)";

void RoleRepository::bindRole(QSqlQuery &q, Core::Snowflake guildId, const Discord::Role &role)
{
    q.bindValue(":id", static_cast<qint64>(role.id.get()));
    q.bindValue(":guild_id", static_cast<qint64>(guildId));
    q.bindValue(":name", role.name.get());
    q.bindValue(":permissions", static_cast<qint64>(role.permissions.get()));
    q.bindValue(":position", role.position.get());
    bindOptional(q, ":color", role.color);
    bindOptional(q, ":hoist", role.hoist);
    bindOptional(q, ":icon", role.icon);
    bindOptional(q, ":unicode_emoji", role.unicodeEmoji);
    bindOptional(q, ":managed", role.managed);
    bindOptional(q, ":mentionable", role.mentionable);
}

void RoleRepository::saveRole(Core::Snowflake guildId, const Discord::Role &role, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare(ROLE_UPSERT_SQL);
    bindRole(q, guildId, role);
    execLogged(q, "RoleRepository: Save role");
}

void RoleRepository::saveRoles(Core::Snowflake guildId, const QList<Discord::Role> &roles,
                               QSqlDatabase &db)
{
    if (roles.isEmpty())
        return;

    QSqlQuery q(db);
    q.prepare(ROLE_UPSERT_SQL);

    for (const auto &role : roles) {
        bindRole(q, guildId, role);
        execLogged(q, "RoleRepository: Save role");
    }
}

std::optional<Discord::Role> RoleRepository::getRole(Core::Snowflake guildId,
                                                     Core::Snowflake roleId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, name, permissions, position, color, hoist, icon,
               unicode_emoji, managed, mentionable
        FROM roles WHERE guild_id = :guild_id AND id = :id
    )");
    q.bindValue(":guild_id", static_cast<qint64>(guildId));
    q.bindValue(":id", static_cast<qint64>(roleId));

    if (!q.exec() || !q.next())
        return std::nullopt;

    return readRoleFromQuery(q);
}

QList<Discord::Role> RoleRepository::getRolesForGuild(Core::Snowflake guildId)
{
    QList<Discord::Role> roles;
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, name, permissions, position, color, hoist, icon,
               unicode_emoji, managed, mentionable
        FROM roles WHERE guild_id = :guild_id
        ORDER BY position ASC
    )");
    q.bindValue(":guild_id", static_cast<qint64>(guildId));

    if (!execLogged(q, "RoleRepository: Get roles"))
        return roles;

    while (q.next())
        roles.append(readRoleFromQuery(q));

    return roles;
}

void RoleRepository::deleteRole(Core::Snowflake guildId, Core::Snowflake roleId, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM roles WHERE guild_id = :guild_id AND id = :id");
    q.bindValue(":guild_id", static_cast<qint64>(guildId));
    q.bindValue(":id", static_cast<qint64>(roleId));

    execLogged(q, "RoleRepository: Delete role");
}

void RoleRepository::deleteRolesForGuild(Core::Snowflake guildId, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM roles WHERE guild_id = :guild_id");
    q.bindValue(":guild_id", static_cast<qint64>(guildId));

    execLogged(q, "RoleRepository: Delete roles for guild");
}

} // namespace Storage
} // namespace Acheron
