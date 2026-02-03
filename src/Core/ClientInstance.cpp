#include "ClientInstance.hpp"

#include "Core/Logging.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/RoleRepository.hpp"
#include "Storage/MemberRepository.hpp"

namespace Acheron {
namespace Core {
ClientInstance::ClientInstance(const AccountInfo &info, QObject *parent)
    : QObject(parent),
      account(info),
      roleRepo(info.id),
      guildRepo(info.id),
      channelRepo(info.id),
      memberRepo(info.id)
{
    client = new Discord::Client(info.token, info.gatewayUrl, info.restUrl, this);

    Storage::DatabaseManager::instance().openCacheDatabase(info.id);

    userManager = new UserManager(info.id, this);
    messageManager = new MessageManager(info.id, client, userManager, this);

    permissionManager = new PermissionManager(info.id, this);

    connect(client, &Discord::Client::stateChanged, this, &ClientInstance::stateChanged);

    connect(client, &Discord::Client::ready, this, [this](const Discord::Ready &ready) {
        QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
        QSqlDatabase db = QSqlDatabase::database(connName);

        account.username = ready.user->username;
        account.displayName = ready.user->globalName;
        account.avatar = ready.user->avatar;

        db.transaction();
        for (size_t i = 0; i < ready.guilds->size(); i++) {
            const auto &guild = ready.guilds->at(i);

            const Discord::Member *me = nullptr;
            if (ready.mergedMembers.hasValue()) {
                const auto &members = ready.mergedMembers->at(i);
                for (const auto &member : members) {
                    memberRepo.saveMember(guild.properties->id.get(), member.userId.get(), member);
                    if (member.userId.get() == ready.user->id.get())
                        me = &member;
                }
            }

            if (me && guild.roles.hasValue() && guild.channels.hasValue())
                permissionManager->precomputeGuildPermissions(guild.asGuild(), *me,
                                                              guild.roles.get(),
                                                              guild.channels.get(), ready.user->id);

            guildRepo.saveGuild(guild.asGuild(), db);

            if (guild.roles.hasValue())
                roleRepo.saveRoles(guild.properties->id.get(), guild.roles.get(), db);

            for (const auto &channel : guild.channels.get()) {
                // we dont get a guild_id from the gateway here
                Discord::Channel copy = channel;
                copy.guildId = guild.properties->id;
                channelRepo.saveChannel(copy, db);

                if (copy.permissionOverwrites.hasValue())
                    channelRepo.savePermissionOverwrites(copy.id.get(),
                                                         copy.permissionOverwrites.get(), db);
            }
        }

        userManager->saveUser(ready.user);

        db.commit();

        emit detailsUpdated(account);
        emit this->ready(ready);
    });

    connect(client, &Discord::Client::readySupplemental, this,
            [this](const Discord::ReadySupplemental &data) {
                QString connName =
                        Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
                QSqlDatabase db = QSqlDatabase::database(connName);

                db.transaction();

                for (size_t i = 0; i < data.mergedMembers->size(); i++) {
                    const auto &guild = data.guilds->at(i);
                    const auto &members = data.mergedMembers->at(i);

                    for (const auto &member : members)
                        memberRepo.saveMember(guild.id, member.userId.get(), member);
                }

                db.commit();
            });

    connect(client, &Discord::Client::messageCreated, messageManager,
            &MessageManager::onMessageCreated);
    connect(client, &Discord::Client::messageUpdated, messageManager,
            &MessageManager::onMessageUpdated);
    connect(client, &Discord::Client::messageDeleted, messageManager,
            &MessageManager::onMessageDeleted);
    connect(client, &Discord::Client::messageSendFailed, messageManager,
            &MessageManager::onMessageSendFailed);
    connect(client, &Discord::Client::channelCreated, this, &ClientInstance::onChannelCreated);
    connect(client, &Discord::Client::channelUpdated, this, &ClientInstance::onChannelUpdated);
    connect(client, &Discord::Client::channelDeleted, this, &ClientInstance::onChannelDeleted);
    connect(client, &Discord::Client::guildMembersChunk, this,
            &ClientInstance::onGuildMembersChunk);
    connect(messageManager, &MessageManager::messagesReceived, this,
            &ClientInstance::onMessagesReceived);
}

void ClientInstance::onChannelCreated(const Discord::ChannelCreate &event)
{
    if (!event.channel.hasValue())
        return;

    const Discord::Channel &channel = event.channel.get();
    Core::Snowflake channelId = channel.id.get();

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    db.transaction();

    channelRepo.saveChannel(channel, db);

    if (channel.permissionOverwrites.hasValue())
        channelRepo.savePermissionOverwrites(channelId, channel.permissionOverwrites.get(), db);

    db.commit();

    emit channelCreated(event);
}

void ClientInstance::onChannelUpdated(const Discord::ChannelUpdate &event)
{
    if (!event.channel.hasValue())
        return;

    const Discord::Channel &channel = event.channel.get();
    Core::Snowflake channelId = channel.id.get();

    qCDebug(LogCore) << "Channel updated:" << channelId;

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    db.transaction();

    channelRepo.saveChannel(channel, db);

    if (channel.permissionOverwrites.hasValue())
        channelRepo.savePermissionOverwrites(channelId, channel.permissionOverwrites.get(), db);

    db.commit();

    permissionManager->invalidateChannelCache(channelId);

    emit channelUpdated(event);
}

void ClientInstance::onChannelDeleted(const Discord::ChannelDelete &event)
{
    if (!event.id.hasValue())
        return;

    Core::Snowflake channelId = event.id.get();

    qCDebug(LogCore) << "Channel deleted:" << channelId;

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    db.transaction();

    channelRepo.deleteChannel(channelId, db);

    db.commit();

    permissionManager->invalidateChannelCache(channelId);

    emit channelDeleted(event);
}

void ClientInstance::onGuildMembersChunk(const Discord::GuildMembersChunk &chunk)
{
    Snowflake guildId = chunk.guildId.get();
    QList<Snowflake> updatedUserIds;

    for (const auto &member : chunk.members.get()) {
        Snowflake userId =
                member.userId.hasValue()
                        ? member.userId.get()
                        : (member.user.hasValue() ? member.user->id.get() : Snowflake::Invalid);
        if (!userId.isValid())
            continue;

        memberRepo.saveMember(guildId, userId, member);
        if (member.user.hasValue())
            userManager->saveUser(member.user.get());

        pendingMemberRequests.remove(qMakePair(guildId, userId));
        updatedUserIds.append(userId);
    }

    if (!updatedUserIds.isEmpty())
        emit membersUpdated(guildId, updatedUserIds);
}

void ClientInstance::onMessagesReceived(const MessageRequestResult &result)
{
    if (!result.success || result.messages.isEmpty())
        return;

    if (result.type != Discord::Client::MessageLoadType::Latest &&
        result.type != Discord::Client::MessageLoadType::History)
        return;

    auto channelOpt = channelRepo.getChannel(result.channelId);
    if (!channelOpt || !channelOpt->guildId.hasValue())
        return;

    Snowflake guildId = channelOpt->guildId.get();
    QList<Snowflake> missingUserIds;

    for (const auto &msg : result.messages) {
        if (!msg.author.hasValue())
            continue;

        Snowflake userId = msg.author->id.get();

        if (pendingMemberRequests.contains(qMakePair(guildId, userId)))
            continue;

        if (userManager->getMember(guildId, userId))
            continue;

        pendingMemberRequests.insert(qMakePair(guildId, userId));
        missingUserIds.append(userId);
    }

    if (!missingUserIds.isEmpty())
        client->requestGuildMembers(guildId, missingUserIds);
}

ClientInstance::~ClientInstance()
{
    Storage::DatabaseManager::instance().closeCacheDatabase(account.id);
}

void ClientInstance::start()
{
    discord()->start();
}

void ClientInstance::stop()
{
    discord()->stop();
}

Discord::Client *ClientInstance::discord() const
{
    return client;
}

MessageManager *ClientInstance::messages() const
{
    return messageManager;
}

UserManager *ClientInstance::users() const
{
    return userManager;
}

PermissionManager *ClientInstance::permissions() const
{
    return permissionManager;
}

QList<Discord::Role> ClientInstance::getRolesForGuild(Snowflake guildId)
{
    return roleRepo.getRolesForGuild(guildId);
}

ConnectionState ClientInstance::state() const
{
    return client->getState();
}

Snowflake ClientInstance::accountId() const
{
    return account.id;
}

const AccountInfo &ClientInstance::accountInfo() const
{
    return account;
}
} // namespace Core
} // namespace Acheron
