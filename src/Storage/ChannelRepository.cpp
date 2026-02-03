#include "ChannelRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

#include <QSqlQuery>
#include <QSqlError>

namespace Acheron {
namespace Storage {

ChannelRepository::ChannelRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void ChannelRepository::saveChannel(const Discord::Channel &channel, QSqlDatabase &db)
{
    QSqlQuery q(db);

    q.prepare(R"(
		INSERT OR REPLACE INTO channels
		(id, type, position, name, guild_id, parent_id, last_message_id, icon, owner_id)
		VALUES (:id, :type, :position, :name, :guild_id, :parent_id, :last_message_id, :icon, :owner_id)
    )");

    q.bindValue(":id", static_cast<qint64>(channel.id.get()));
    q.bindValue(":type", static_cast<qint64>(channel.type.get()));
    q.bindValue(":position", static_cast<qint64>(channel.position.get()));
    q.bindValue(":name", channel.name);
    if (channel.guildId.hasValue())
        q.bindValue(":guild_id", static_cast<qint64>(channel.guildId.get()));
    if (channel.parentId.hasValue())
        q.bindValue(":parent_id", static_cast<qint64>(channel.parentId.get()));
    if (channel.lastMessageId.hasValue())
        q.bindValue(":last_message_id", static_cast<qint64>(channel.lastMessageId.get()));
    if (channel.icon.hasValue())
        q.bindValue(":icon", channel.icon.get());
    if (channel.ownerId.hasValue())
        q.bindValue(":owner_id", static_cast<qint64>(channel.ownerId.get()));

    if (!q.exec())
        qCWarning(LogDB) << "ChannelRepository: Save failed:" << q.lastError().text();
}

void ChannelRepository::deleteChannel(Core::Snowflake channelId, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM permission_overwrites WHERE channel_id = :channel_id");
    q.bindValue(":channel_id", static_cast<qint64>(channelId));
    if (!q.exec())
        qCWarning(LogDB) << "ChannelRepository: Delete overwrites failed:" << q.lastError().text();

    q.prepare("DELETE FROM channel_recipients WHERE channel_id = :channel_id");
    q.bindValue(":channel_id", static_cast<qint64>(channelId));
    if (!q.exec())
        qCWarning(LogDB) << "ChannelRepository: Delete recipients failed:" << q.lastError().text();

    q.prepare("DELETE FROM channels WHERE id = :id");
    q.bindValue(":id", static_cast<qint64>(channelId));
    if (!q.exec())
        qCWarning(LogDB) << "ChannelRepository: Delete channel failed:" << q.lastError().text();
}

void ChannelRepository::savePermissionOverwrites(
        Core::Snowflake channelId, const QList<Discord::PermissionOverwrite> &overwrites,
        QSqlDatabase &db)
{
    QSqlQuery delQ(db);
    delQ.prepare("DELETE FROM permission_overwrites WHERE channel_id = :channel_id");
    delQ.bindValue(":channel_id", static_cast<qint64>(channelId));
    delQ.exec();

    if (overwrites.isEmpty())
        return;

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT INTO permission_overwrites
        (channel_id, target_id, type, allow, deny)
        VALUES (:channel_id, :target_id, :type, :allow, :deny)
    )");

    for (const auto &ow : overwrites) {
        q.bindValue(":channel_id", static_cast<qint64>(channelId));
        q.bindValue(":target_id", static_cast<qint64>(ow.id.get()));
        q.bindValue(":type", static_cast<int>(ow.type.get()));
        q.bindValue(":allow", static_cast<qint64>(ow.allow.get()));
        q.bindValue(":deny", static_cast<qint64>(ow.deny.get()));

        if (!q.exec())
            qCWarning(LogDB) << "ChannelRepository: Save overwrite failed:" << q.lastError().text();
    }
}

void ChannelRepository::saveChannelRecipients(Core::Snowflake channelId,
                                              const QList<Core::Snowflake> &recipientIds,
                                              QSqlDatabase &db)
{
    QSqlQuery delQ(db);
    delQ.prepare("DELETE FROM channel_recipients WHERE channel_id = :channel_id");
    delQ.bindValue(":channel_id", static_cast<qint64>(channelId));
    delQ.exec();

    if (recipientIds.isEmpty())
        return;

    QSqlQuery q(db);
    q.prepare(R"(
        INSERT INTO channel_recipients
        (channel_id, user_id)
        VALUES (:channel_id, :user_id)
    )");

    for (const auto &userId : recipientIds) {
        q.bindValue(":channel_id", static_cast<qint64>(channelId));
        q.bindValue(":user_id", static_cast<qint64>(userId));

        if (!q.exec())
            qCWarning(LogDB) << "ChannelRepository: Save recipient failed:" << q.lastError().text();
    }
}

QList<Discord::PermissionOverwrite> ChannelRepository::getPermissionOverwrites(
        Core::Snowflake channelId)
{
    QList<Discord::PermissionOverwrite> overwrites;
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT target_id, type, allow, deny
        FROM permission_overwrites WHERE channel_id = :channel_id
    )");
    q.bindValue(":channel_id", static_cast<qint64>(channelId));

    if (!q.exec())
        return overwrites;

    while (q.next()) {
        Discord::PermissionOverwrite ow;
        ow.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
        ow.type = static_cast<Discord::PermissionOverwrite::Type>(q.value(1).toInt());
        ow.allow = Discord::Permissions::fromInt(q.value(2).toLongLong());
        ow.deny = Discord::Permissions::fromInt(q.value(3).toLongLong());
        overwrites.append(ow);
    }

    return overwrites;
}

QList<Core::Snowflake> ChannelRepository::getChannelRecipientIds(Core::Snowflake channelId)
{
    QList<Core::Snowflake> recipientIds;
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT user_id
        FROM channel_recipients WHERE channel_id = :channel_id
    )");
    q.bindValue(":channel_id", static_cast<qint64>(channelId));

    if (!q.exec())
        return recipientIds;

    while (q.next())
        recipientIds.append(static_cast<Core::Snowflake>(q.value(0).toLongLong()));

    return recipientIds;
}

QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> ChannelRepository::
        getPermissionOverwritesForGuild(Core::Snowflake guildId)
{
    QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> result;
    auto db = getDb();

    QSqlQuery channelIdsQuery(db);
    channelIdsQuery.prepare("SELECT id FROM channels WHERE guild_id = :guild_id");
    channelIdsQuery.bindValue(":guild_id", static_cast<qint64>(guildId));

    if (!channelIdsQuery.exec())
        return result;

    QList<Core::Snowflake> channelIds;
    while (channelIdsQuery.next())
        channelIds.append(static_cast<Core::Snowflake>(channelIdsQuery.value(0).toLongLong()));

    if (channelIds.isEmpty())
        return result;

    QString placeholders;
    for (int i = 0; i < channelIds.size(); ++i) {
        if (i > 0)
            placeholders += ",";
        placeholders += QString(":id%1").arg(i);
    }

    // clang-format off
    QSqlQuery q(db);
    q.prepare(QString(R"(
        SELECT channel_id, target_id, type, allow, deny
        FROM permission_overwrites
        WHERE channel_id IN (%1)
    )").arg(placeholders));
    // clang-format on

    for (int i = 0; i < channelIds.size(); ++i)
        q.bindValue(QString(":id%1").arg(i), static_cast<qint64>(channelIds[i]));

    if (!q.exec())
        return result;

    while (q.next()) {
        Core::Snowflake channelId = static_cast<Core::Snowflake>(q.value(0).toLongLong());
        Discord::PermissionOverwrite ow;
        ow.id = static_cast<Core::Snowflake>(q.value(1).toLongLong());
        ow.type = static_cast<Discord::PermissionOverwrite::Type>(q.value(2).toInt());
        ow.allow = Discord::Permissions::fromInt(q.value(3).toLongLong());
        ow.deny = Discord::Permissions::fromInt(q.value(4).toLongLong());
        result[channelId].append(ow);
    }

    return result;
}

std::optional<Discord::Channel> ChannelRepository::getChannel(Core::Snowflake channelId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, type, position, name, guild_id, parent_id, last_message_id, icon, owner_id
        FROM channels WHERE id = :id
    )");
    q.bindValue(":id", static_cast<qint64>(channelId));

    if (!q.exec() || !q.next())
        return std::nullopt;

    Discord::Channel channel;
    channel.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
    channel.type = static_cast<Discord::ChannelType>(q.value(1).toInt());
    if (!q.value(2).isNull())
        channel.position = q.value(2).toInt();
    if (!q.value(3).isNull())
        channel.name = q.value(3).toString();
    if (!q.value(4).isNull())
        channel.guildId = static_cast<Core::Snowflake>(q.value(4).toLongLong());
    if (!q.value(5).isNull())
        channel.parentId = static_cast<Core::Snowflake>(q.value(5).toLongLong());
    if (!q.value(6).isNull())
        channel.lastMessageId = static_cast<Core::Snowflake>(q.value(6).toLongLong());
    if (!q.value(7).isNull())
        channel.icon = q.value(7).toString();
    if (!q.value(8).isNull())
        channel.ownerId = static_cast<Core::Snowflake>(q.value(8).toLongLong());

    channel.permissionOverwrites = getPermissionOverwrites(channelId);

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM)
        channel.recipientIds = getChannelRecipientIds(channelId);

    return channel;
}

} // namespace Storage
} // namespace Acheron
