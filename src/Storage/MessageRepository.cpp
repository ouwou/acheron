#include "MessageRepository.hpp"

#include <QJsonDocument>
#include <QJsonArray>

#include "DatabaseManager.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

MessageRepository::MessageRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId)), userRepository(accountId)
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
		(id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds, reactions, deleted,
		 referenced_message_id)
		VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds, :reactions, 0,
		        :ref_msg_id)
    )");

    QSqlQuery qAtt(db);
    qAtt.prepare(R"(
        INSERT OR REPLACE INTO attachments
        (id, message_id, filename, content_type, size, url, proxy_url, width, height)
        VALUES (:id, :message_id, :filename, :content_type, :size, :url, :proxy_url, :width, :height)
    )");

    // Collect referenced messages to save as their own rows
    QList<Discord::Message> referencedMessages;

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
        qMsg.bindValue(":reactions", message.reactionsJson.isEmpty() ? QVariant() : message.reactionsJson);

        if (message.referencedMessage) {
            qMsg.bindValue(":ref_msg_id", static_cast<qint64>(message.referencedMessage->id.get()));
            referencedMessages.append(*message.referencedMessage);
        } else if (message.messageReference.hasValue() && message.messageReference->messageId.hasValue()) {
            qMsg.bindValue(":ref_msg_id", static_cast<qint64>(message.messageReference->messageId.get()));
        } else {
            qMsg.bindValue(":ref_msg_id", QVariant());
        }

        if (!qMsg.exec()) {
            qCWarning(LogDB) << "MessageRepository: Save messages failed:"
                             << qMsg.lastError().text();
        }

        userRepository.saveUser(message.author.get(), db);

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

    // Save referenced messages as their own rows (INSERT OR IGNORE to not overwrite
    // a potentially more complete version already in the DB)
    if (!referencedMessages.isEmpty()) {
        QSqlQuery qRef(db);
        qRef.prepare(R"(
            INSERT OR IGNORE INTO messages
            (id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds, deleted)
            VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds, 0)
        )");

        for (const auto &ref : referencedMessages) {
            qRef.bindValue(":id", static_cast<qint64>(ref.id.get()));
            qRef.bindValue(":channel_id", static_cast<qint64>(ref.channelId.get()));
            qRef.bindValue(":author_id", static_cast<qint64>(ref.author->id.get()));
            qRef.bindValue(":content", ref.content);
            qRef.bindValue(":timestamp", ref.timestamp);
            qRef.bindValue(":edited_timestamp", ref.editedTimestamp);
            qRef.bindValue(":type", static_cast<qint64>(ref.type.get()));
            qRef.bindValue(":flags", static_cast<qint64>(ref.flags.get()));
            qRef.bindValue(":embeds", ref.embedsJson.isEmpty() ? QVariant() : ref.embedsJson);

            if (!qRef.exec()) {
                qCWarning(LogDB) << "MessageRepository: Save referenced message failed:"
                                 << qRef.lastError().text();
            }

            userRepository.saveUser(ref.author.get(), db);
        }
    }

    db.commit();
}

void MessageRepository::markMessageDeleted(Core::Snowflake messageId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        UPDATE messages SET deleted = 1 WHERE id = :id
    )");
    q.bindValue(":id", static_cast<qint64>(messageId));

    if (!q.exec())
        qCWarning(LogDB) << "MessageRepository: Mark message deleted failed:" << q.lastError().text();
}

void MessageRepository::updateReactionsJson(Core::Snowflake messageId, const QString &reactionsJson)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        UPDATE messages SET reactions = :reactions WHERE id = :id
    )");
    q.bindValue(":reactions", reactionsJson.isEmpty() ? QVariant() : reactionsJson);
    q.bindValue(":id", static_cast<qint64>(messageId));

    if (!q.exec())
        qCWarning(LogDB) << "MessageRepository: Update reactions failed:" << q.lastError().text();
}

QString MessageRepository::getReactionsJson(Core::Snowflake messageId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT reactions FROM messages WHERE id = :id
    )");
    q.bindValue(":id", static_cast<qint64>(messageId));

    if (!q.exec() || !q.next())
        return {};

    return q.value(0).toString();
}

QList<Discord::Message> MessageRepository::getLatestMessages(Core::Snowflake channelId, int limit)
{
    auto db = getDb();

    QList<Discord::Message> messages;
    QSqlQuery q(db);
    q.prepare(R"(
		SELECT m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds, m.reactions,
               u.id, u.username, u.global_name, u.avatar, u.bot,
               m.referenced_message_id,
               rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds,
               ru.id, ru.username, ru.global_name, ru.avatar, ru.bot
		FROM messages m
        INNER JOIN users u ON m.author_id = u.id
        LEFT JOIN messages rm ON m.referenced_message_id = rm.id
        LEFT JOIN users ru ON rm.author_id = ru.id
		WHERE m.channel_id = :channel_id AND m.deleted = 0
		ORDER BY m.id DESC
        LIMIT :limit
    )");

    q.bindValue(":channel_id", static_cast<qint64>(channelId));
    q.bindValue(":limit", limit);

    if (!q.exec()) {
        qCWarning(LogDB) << "MessageRepository: Get messages failed:" << q.lastError().text();
        return messages;
    }

    while (q.next()) {
        Discord::Message message = readMessageFromQuery(q);
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
		SELECT m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds, m.reactions,
			   u.id, u.username, u.global_name, u.avatar, u.bot,
			   m.referenced_message_id,
			   rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds,
			   ru.id, ru.username, ru.global_name, ru.avatar, ru.bot
		FROM messages m
		INNER JOIN users u ON m.author_id = u.id
		LEFT JOIN messages rm ON m.referenced_message_id = rm.id
		LEFT JOIN users ru ON rm.author_id = ru.id
		WHERE m.channel_id = :channel_id AND m.id < :before_id AND m.deleted = 0
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
        Discord::Message message = readMessageFromQuery(q);
        messages.append(message);
    }

    loadAttachmentsForMessages(messages, db);

    return messages;
}

Discord::Message MessageRepository::readMessageFromQuery(const QSqlQuery &q)
{
    // Columns 0-8: m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds
    // Column 9: m.reactions
    // Columns 10-14: u.id, u.username, u.global_name, u.avatar, u.bot
    // Column 15: m.referenced_message_id
    // Columns 16-24: rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds
    // Columns 25-29: ru.id, ru.username, ru.global_name, ru.avatar, ru.bot

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

    QString reactionsJson = q.value(9).toString();
    if (!reactionsJson.isEmpty()) {
        message.reactionsJson = reactionsJson;
        QJsonDocument doc = QJsonDocument::fromJson(reactionsJson.toUtf8());
        if (doc.isArray()) {
            QList<Discord::Reaction> reactionList;
            for (const QJsonValue &val : doc.array())
                reactionList.append(Discord::Reaction::fromJson(val.toObject()));
            message.reactions = reactionList;
        }
    }

    message.author->id = static_cast<Core::Snowflake>(q.value(10).toLongLong());
    message.author->username = q.value(11).toString();
    message.author->globalName = q.value(12).toString();
    message.author->avatar = q.value(13).toString();
    message.author->bot = q.value(14).toBool();

    // Load referenced message from the self-join
    if (!q.value(15).isNull()) {
        Discord::MessageReference ref;
        ref.messageId = static_cast<Core::Snowflake>(q.value(15).toLongLong());
        ref.channelId = message.channelId;
        message.messageReference = ref;

        // If the joined message row exists (rm.id is not null), reconstruct it
        if (!q.value(16).isNull()) {
            auto refMsg = std::make_shared<Discord::Message>();
            refMsg->id = static_cast<Core::Snowflake>(q.value(16).toLongLong());
            refMsg->channelId = static_cast<Core::Snowflake>(q.value(17).toLongLong());
            refMsg->content = q.value(19).toString();
            refMsg->timestamp = q.value(20).toDateTime();
            refMsg->editedTimestamp = q.value(21).toDateTime();
            refMsg->type = static_cast<Discord::MessageType>(q.value(22).toLongLong());
            refMsg->flags = static_cast<Discord::MessageFlags>(q.value(23).toLongLong());

            QString refEmbedsJson = q.value(24).toString();
            if (!refEmbedsJson.isEmpty()) {
                refMsg->embedsJson = refEmbedsJson;
                QJsonDocument doc = QJsonDocument::fromJson(refEmbedsJson.toUtf8());
                if (doc.isArray()) {
                    QList<Discord::Embed> embedList;
                    for (const QJsonValue &val : doc.array())
                        embedList.append(Discord::Embed::fromJson(val.toObject()));
                    refMsg->embeds = embedList;
                }
            }

            if (!q.value(25).isNull()) {
                refMsg->author->id = static_cast<Core::Snowflake>(q.value(25).toLongLong());
                refMsg->author->username = q.value(26).toString();
                refMsg->author->globalName = q.value(27).toString();
                refMsg->author->avatar = q.value(28).toString();
                refMsg->author->bot = q.value(29).toBool();
            }

            message.referencedMessage = refMsg;
        }
    }

    return message;
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
