#pragma once

#include <optional>

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
    void updateMessageContent(const Discord::Message &message);
    void updateReactionsJson(Core::Snowflake messageId, const QString &reactionsJson);
    QString getReactionsJson(Core::Snowflake messageId);
    std::optional<Discord::Message> getMessage(Core::Snowflake messageId);

    QList<Discord::Message> getMessagesInRange(Core::Snowflake channelId, Core::Snowflake fromId, Core::Snowflake toId);

private:
    void loadAttachmentsForMessages(QList<Discord::Message> &messages, QSqlDatabase &db);
    Discord::Message readMessageFromQuery(const QSqlQuery &q);

    UserRepository userRepository;
};

} // namespace Storage
} // namespace Acheron
