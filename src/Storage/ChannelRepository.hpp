#pragma once

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
};

} // namespace Storage
} // namespace Acheron
