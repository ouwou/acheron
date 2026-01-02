#pragma once

#include <QSqlDatabase>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Acheron {
namespace Storage {

class MessageRepository : public QObject, public BaseRepository
{
    Q_OBJECT
public:
    MessageRepository(Core::Snowflake accountId);

    void saveMessages(const QList<Discord::Message> &messages);
    void saveMessages(const QList<Discord::Message> &messages, QSqlDatabase &db);

    QList<Discord::Message> getLatestMessages(Core::Snowflake channelId, int limit);
    QList<Discord::Message> getMessagesBefore(Core::Snowflake channelId, Core::Snowflake beforeId,
                                              int limit);
};

} // namespace Storage
} // namespace Acheron
