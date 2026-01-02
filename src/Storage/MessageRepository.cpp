#include "MessageRepository.hpp"

#include "DatabaseManager.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

MessageRepository::MessageRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void MessageRepository::saveMessages(const QList<Discord::Message> &messages)
{
    auto db = getDb();
    saveMessages(messages, db);
}

void MessageRepository::saveMessages(const QList<Discord::Message> &messages, QSqlDatabase &db)
{
    if (messages.isEmpty())
        return;

    db.transaction();
    QSqlQuery qMsg(db);
    qMsg.prepare(R"(
        INSERT OR REPLACE INTO messages
		(id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags)
		VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags)
    )");

    QSqlQuery qUser(db);
    qUser.prepare(R"(
		INSERT OR REPLACE INTO users
		(id, username, global_name, avatar, bot)
		VALUES (:id, :username, :global_name, :avatar, :bot)
	)");

    for (const auto &message : messages) {
        qMsg.bindValue(":id", static_cast<qint64>(message.id.get()));
        qMsg.bindValue(":channel_id", static_cast<qint64>(message.channelId.get()));
        qMsg.bindValue(":author_id", static_cast<qint64>(message.author->id.get()));
        qMsg.bindValue(":content", message.content);
        qMsg.bindValue(":timestamp", message.timestamp);
        qMsg.bindValue(":edited_timestamp", message.editedTimestamp);
        qMsg.bindValue(":type", static_cast<qint64>(message.type.get()));
        qMsg.bindValue(":flags", static_cast<qint64>(message.flags.get()));

        if (!qMsg.exec()) {
            qCWarning(LogDB) << "MessageRepository: Save messages failed:"
                             << qMsg.lastError().text();
        }

        qUser.bindValue(":id", static_cast<qint64>(message.author->id.get()));
        qUser.bindValue(":username", message.author->username);
        qUser.bindValue(":global_name", message.author->globalName);
        qUser.bindValue(":avatar", message.author->avatar);
        qUser.bindValue(":bot", static_cast<qint64>(message.author->bot.get()));

        if (!qUser.exec()) {
            qCWarning(LogDB) << "MessageRepository: Save user failed:" << qUser.lastError().text();
        }
    }

    db.commit();
}

QList<Discord::Message> MessageRepository::getLatestMessages(Core::Snowflake channelId, int limit)
{
    auto db = getDb();

    QList<Discord::Message> messages;
    QSqlQuery q(db);
    q.prepare(R"(
		SELECT m.id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags,
               u.id, u.username, u.global_name, u.avatar, u.bot
		FROM messages m
        INNER JOIN users u
		ON m.author_id = u.id
		WHERE channel_id = :channel_id
		ORDER BY m.id ASC
        LIMIT :limit
    )");

    q.bindValue(":channel_id", static_cast<qint64>(channelId));
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        qCWarning(LogDB) << "MessageRepository: Get messages failed:" << q.lastError().text();
        return messages;
    }

    while (q.next()) {
        Discord::Message message;
        message.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
        message.channelId = static_cast<Core::Snowflake>(q.value(1).toLongLong());
        message.content = q.value(3).toString();
        message.timestamp = q.value(4).toDateTime();
        message.editedTimestamp = q.value(5).toDateTime();
        message.type = static_cast<Discord::MessageType>(q.value(6).toLongLong());
        message.flags = static_cast<Discord::MessageFlags>(q.value(7).toLongLong());

        message.author->id = static_cast<Core::Snowflake>(q.value(8).toLongLong());
        message.author->username = q.value(9).toString();
        message.author->globalName = q.value(10).toString();
        message.author->avatar = q.value(11).toString();
        message.author->bot = q.value(12).toBool();

        messages.append(message);
    }

    return messages;
}

QList<Discord::Message> MessageRepository::getMessagesBefore(Core::Snowflake channelId,
                                                             Core::Snowflake beforeId, int limit)
{
    auto db = getDb();

    QList<Discord::Message> messages;
    QSqlQuery q(db);
    q.prepare(R"(
		SELECT m.id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags,
			   u.id, u.username, u.global_name, u.avatar, u.bot
		FROM messages m
		INNER JOIN users u
		ON m.author_id = u.id
		WHERE channel_id = :channel_id AND m.id < :before_id
		ORDER BY m.id DESC
		LIMIT :limit
	)");

    q.bindValue(":channel_id", static_cast<qint64>(channelId));
    q.bindValue(":before_id", static_cast<qint64>(beforeId));
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        qCWarning(LogDB) << "MessageRepository: Get messages failed:" << q.lastError().text();
        return messages;
    }

    while (q.next()) {
        Discord::Message message;
        message.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
        message.channelId = static_cast<Core::Snowflake>(q.value(1).toLongLong());
        message.content = q.value(3).toString();
        message.timestamp = q.value(4).toDateTime();
        message.editedTimestamp = q.value(5).toDateTime();
        message.type = static_cast<Discord::MessageType>(q.value(6).toLongLong());
        message.flags = static_cast<Discord::MessageFlags>(q.value(7).toLongLong());

        message.author->id = static_cast<Core::Snowflake>(q.value(8).toLongLong());
        message.author->username = q.value(9).toString();
        message.author->globalName = q.value(10).toString();
        message.author->avatar = q.value(11).toString();
        message.author->bot = q.value(12).toBool();

        messages.append(message);
    }

    return messages;
}

} // namespace Storage
} // namespace Acheron
