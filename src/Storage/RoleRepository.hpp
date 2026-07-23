#pragma once

#include <QSqlDatabase>
#include <optional>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Storage {

class RoleRepository : public BaseRepository
{
public:
    RoleRepository(Core::Snowflake accountId);

    void saveRole(Core::Snowflake guildId, const Discord::Role &role, QSqlDatabase &db);
    void saveRoles(Core::Snowflake guildId, const QList<Discord::Role> &roles, QSqlDatabase &db);
    void deleteRole(Core::Snowflake guildId, Core::Snowflake roleId, QSqlDatabase &db);
    void deleteRolesForGuild(Core::Snowflake guildId, QSqlDatabase &db);

    std::optional<Discord::Role> getRole(Core::Snowflake guildId, Core::Snowflake roleId);
    QList<Discord::Role> getRolesForGuild(Core::Snowflake guildId);

private:
    static void bindRole(QSqlQuery &q, Core::Snowflake guildId, const Discord::Role &role);
};

} // namespace Storage
} // namespace Acheron
