#include "MessageRepository.hpp"

#include <QJsonDocument>
#include <QJsonArray>

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
		(id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds)
		VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds)
    )");

    QSqlQuery qUser(db);
    qUser.prepare(R"(
		INSERT OR REPLACE INTO users
		(id, username, global_name, avatar, bot)
		VALUES (:id, :username, :global_name, :avatar, :bot)
	)");

    QSqlQuery qAtt(db);
    qAtt.prepare(R"(
        INSERT OR REPLACE INTO attachments
        (id, message_id, filename, content_type, size, url, proxy_url, width, height)
        VALUES (:id, :message_id, :filename, :content_type, :size, :url, :proxy_url, :width, :height)
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
        qMsg.bindValue(":embeds", message.embedsJson.isEmpty() ? QVariant() : message.embedsJson);

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

        if (message.attachments.hasValue()) {
            for (const auto &att : *message.attachments) {
                qAtt.bindValue(":id", static_cast<qint64>(att.id.get()));
                qAtt.bindValue(":message_id", static_cast<qint64>(message.id.get()));
                qAtt.bindValue(":filename", att.filename);
                qAtt.bindValue(":content_type", att.contentType);
                qAtt.bindValue(":size", static_cast<qint64>(att.size.get()));
                qAtt.bindValue(":url", att.url);
                qAtt.bindValue(":proxy_url", att.proxyUrl);
                qAtt.bindValue(":width", att.width.hasValue() ? QVariant(*att.width) : QVariant());
                qAtt.bindValue(":height",
                               att.height.hasValue() ? QVariant(*att.height) : QVariant());

                if (!qAtt.exec()) {
                    qCWarning(LogDB) << "MessageRepository: Save attachment failed:"
                                     << qAtt.lastError().text();
                }
            }
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
		SELECT m.id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds,
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

        QString embedsJson = q.value(8).toString();
        if (!embedsJson.isEmpty()) {
            message.embedsJson = embedsJson;
            QJsonDocument doc = QJsonDocument::fromJson(embedsJson.toUtf8());
            if (doc.isArray()) {
                QList<Discord::Embed> embedList;
                for (const QJsonValue &val : doc.array())
                    embedList.append(Discord::Embed::fromJson(val.toObject()));
                message.embeds = embedList;
            }
        }

        message.author->id = static_cast<Core::Snowflake>(q.value(9).toLongLong());
        message.author->username = q.value(10).toString();
        message.author->globalName = q.value(11).toString();
        message.author->avatar = q.value(12).toString();
        message.author->bot = q.value(13).toBool();

        messages.append(message);
    }

    loadAttachmentsForMessages(messages, db);

    return messages;
}

QList<Discord::Message> MessageRepository::getMessagesBefore(Core::Snowflake channelId,
                                                             Core::Snowflake beforeId, int limit)
{
    auto db = getDb();

    QList<Discord::Message> messages;
    QSqlQuery q(db);
    q.prepare(R"(
		SELECT m.id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds,
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

        QString embedsJson = q.value(8).toString();
        if (!embedsJson.isEmpty()) {
            message.embedsJson = embedsJson;
            QJsonDocument doc = QJsonDocument::fromJson(embedsJson.toUtf8());
            if (doc.isArray()) {
                QList<Discord::Embed> embedList;
                for (const QJsonValue &val : doc.array())
                    embedList.append(Discord::Embed::fromJson(val.toObject()));
                message.embeds = embedList;
            }
        }

        message.author->id = static_cast<Core::Snowflake>(q.value(9).toLongLong());
        message.author->username = q.value(10).toString();
        message.author->globalName = q.value(11).toString();
        message.author->avatar = q.value(12).toString();
        message.author->bot = q.value(13).toBool();

        messages.append(message);
    }

    loadAttachmentsForMessages(messages, db);

    return messages;
}

void MessageRepository::loadAttachmentsForMessages(QList<Discord::Message> &messages,
                                                   QSqlDatabase &db)
{
    if (messages.isEmpty())
        return;

    QHash<qint64, int> messageIndexMap;
    for (int i = 0; i < messages.size(); ++i) {
        messageIndexMap.insert(static_cast<qint64>(messages[i].id.get()), i);
    }

    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, message_id, filename, content_type, size, url, proxy_url, width, height
        FROM attachments
        WHERE message_id IN (SELECT id FROM messages WHERE id IN (%1))
    )");

    QStringList placeholders;
    for (const auto &msg : messages) {
        placeholders.append(QString::number(static_cast<qint64>(msg.id.get())));
    }

    QString query = QString(R"(
        SELECT id, message_id, filename, content_type, size, url, proxy_url, width, height
        FROM attachments
        WHERE message_id IN (%1)
    )")
                            .arg(placeholders.join(", "));

    if (!q.exec(query)) {
        qCWarning(LogDB) << "MessageRepository: Load attachments failed:" << q.lastError().text();
        return;
    }

    while (q.next()) {
        qint64 messageId = q.value(1).toLongLong();
        int idx = messageIndexMap.value(messageId, -1);
        if (idx < 0)
            continue;

        Discord::Attachment att;
        att.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
        att.filename = q.value(2).toString();
        att.contentType = q.value(3).toString();
        att.size = q.value(4).toLongLong();
        att.url = q.value(5).toString();
        att.proxyUrl = q.value(6).toString();
        if (!q.value(7).isNull())
            att.width = q.value(7).toInt();
        if (!q.value(8).isNull())
            att.height = q.value(8).toInt();

        if (!messages[idx].attachments.hasValue())
            messages[idx].attachments = QList<Discord::Attachment>();
        messages[idx].attachments->append(att);
    }
}

} // namespace Storage
} // namespace Acheron
