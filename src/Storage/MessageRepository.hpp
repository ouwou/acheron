#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "UserRepository.hpp"

namespace Acheron {
namespace Storage {

class MessageRepository : public QObject, public BaseRepository
{
    Q_OBJECT
public:
    MessageRepository(Core::Snowflake accountId);

    void saveMessages(const QList<Discord::Message> &messages);
    void saveMessages(const QList<Discord::Message> &messages, QSqlDatabase &db);
    void markMessageDeleted(Core::Snowflake messageId);
    void updateReactionsJson(Core::Snowflake messageId, const QString &reactionsJson);
    QString getReactionsJson(Core::Snowflake messageId);

    QList<Discord::Message> getLatestMessages(Core::Snowflake channelId, int limit);
    QList<Discord::Message> getMessagesBefore(Core::Snowflake channelId, Core::Snowflake beforeId,
                                              int limit);

private:
    void loadAttachmentsForMessages(QList<Discord::Message> &messages, QSqlDatabase &db);
    Discord::Message readMessageFromQuery(const QSqlQuery &q);

    UserRepository userRepository;
};

} // namespace Storage
} // namespace Acheron
