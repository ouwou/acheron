#pragma once

#include <QSqlDatabase>
#include <optional>

#include "Core/Snowflake.hpp"

#include "Discord/Entities.hpp"

#include "BaseRepository.hpp"

namespace Acheron {
namespace Storage {

class GuildRepository : BaseRepository
{
public:
    GuildRepository(Core::Snowflake accountId);

    void saveGuild(const Discord::Guild &guild, QSqlDatabase &db);
    void deleteGuild(Core::Snowflake guildId, QSqlDatabase &db);
    std::optional<Discord::Guild> getGuild(Core::Snowflake guildId);

private:
};

} // namespace Storage
} // namespace Acheron
