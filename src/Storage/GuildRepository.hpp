#pragma once

#include <QSqlDatabase>

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

private:
};

} // namespace Storage
} // namespace Acheron
