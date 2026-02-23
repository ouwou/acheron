#include "ClientInstance.hpp"
#include "ReadStateManager.hpp"
#include "Core/AV/VoiceManager.hpp"

#include <QTimer>

#include "Core/Logging.hpp"
#include "Discord/Enums.hpp"
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
    readStateManager = new ReadStateManager(info.id, permissionManager, this);
    memberListManager = new MemberListManager(channelRepo, roleRepo, this);
    voiceManager = new AV::VoiceManager(info.id, this);

    connect(client, &Discord::Client::stateChanged, this, &ClientInstance::stateChanged);
    connect(client, &Discord::Client::reconnecting, this, &ClientInstance::reconnecting);

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

        if (ready.users.hasValue())
            userManager->saveUsers(ready.users.get());

        if (ready.privateChannels.hasValue()) {
            for (const auto &channel : ready.privateChannels.get()) {
                channelRepo.saveChannel(channel, db);

                QList<Core::Snowflake> recipientIds;
                if (channel.recipients.hasValue()) {
                    for (const auto &user : channel.recipients.get()) {
                        userManager->saveUser(user);
                        recipientIds.append(user.id.get());
                    }
                } else if (channel.recipientIds.hasValue()) {
                    recipientIds = channel.recipientIds.get();
                }

                if (!recipientIds.isEmpty())
                    channelRepo.saveChannelRecipients(channel.id.get(), recipientIds, db);
            }
        }

        db.commit();

        readStateManager->loadFromReady(
                ready.readState.hasValue() ? ready.readState.get()
                                           : QList<Discord::ReadStateEntry>{},
                ready.userGuildSettings.hasValue() ? ready.userGuildSettings.get()
                                                   : QList<Discord::UserGuildSettings>{});

        for (const auto &guild : ready.guilds.get()) {
            for (const auto &channel : guild.channels.get()) {
                if (channel.lastMessageId.hasValue())
                    readStateManager->updateChannelLastMessageId(channel.id.get(),
                                                                 channel.lastMessageId.get());
            }
        }

        if (ready.privateChannels.hasValue()) {
            for (const auto &channel : ready.privateChannels.get()) {
                Snowflake lastMsg = channel.lastMessageId.hasValue()
                                            ? channel.lastMessageId.get()
                                            : channel.id.get();
                readStateManager->updateChannelLastMessageId(channel.id.get(), lastMsg);
            }
        }

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

                for (const auto &guild : data.guilds.get()) {
                    if (!guild.voiceStates.hasValue())
                        continue;
                    for (auto vs : guild.voiceStates.get()) {
                        if (!vs.guildId.hasValue())
                            vs.guildId = guild.id.get();
                        voiceManager->handleVoiceStateUpdate(vs);
                    }
                }
            });

    connect(client, &Discord::Client::messageCreated, messageManager,
            &MessageManager::onMessageCreated);
    connect(client, &Discord::Client::messageUpdated, messageManager,
            &MessageManager::onMessageUpdated);
    connect(client, &Discord::Client::messageDeleted, messageManager,
            &MessageManager::onMessageDeleted);
    connect(client, &Discord::Client::messageSendFailed, messageManager,
            &MessageManager::onMessageSendFailed);
    connect(client, &Discord::Client::messageReactionAdd, messageManager,
            &MessageManager::onReactionAdd);
    connect(client, &Discord::Client::messageReactionAddMany, messageManager,
            &MessageManager::onReactionAddMany);
    connect(client, &Discord::Client::messageReactionRemove, messageManager,
            &MessageManager::onReactionRemove);
    connect(client, &Discord::Client::messageReactionRemoveAll, messageManager,
            &MessageManager::onReactionRemoveAll);
    connect(client, &Discord::Client::messageReactionRemoveEmoji, messageManager,
            &MessageManager::onReactionRemoveEmoji);
    connect(client, &Discord::Client::channelCreated, this, &ClientInstance::onChannelCreated);
    connect(client, &Discord::Client::channelUpdated, this, &ClientInstance::onChannelUpdated);
    connect(client, &Discord::Client::channelDeleted, this, &ClientInstance::onChannelDeleted);
    connect(client, &Discord::Client::guildRoleCreated, this, &ClientInstance::onGuildRoleCreated);
    connect(client, &Discord::Client::guildRoleUpdated, this, &ClientInstance::onGuildRoleUpdated);
    connect(client, &Discord::Client::guildRoleDeleted, this, &ClientInstance::onGuildRoleDeleted);
    connect(client, &Discord::Client::guildMembersChunk, this,
            &ClientInstance::onGuildMembersChunk);
    connect(client, &Discord::Client::guildMemberUpdated, this,
            &ClientInstance::onGuildMemberUpdate);
    connect(client, &Discord::Client::guildMemberListUpdate, memberListManager,
            &MemberListManager::handleMemberListUpdate);
    connect(memberListManager, &MemberListManager::subscriptionRequested, client,
            &Discord::Client::subscribeToGuildChannel);
    connect(messageManager, &MessageManager::messagesReceived, this,
            &ClientInstance::onMessagesReceived);

    connect(client, &Discord::Client::messageAcked, readStateManager,
            &ReadStateManager::onMessageAck);
    connect(client, &Discord::Client::userGuildSettingsUpdated, readStateManager,
            &ReadStateManager::onUserGuildSettingsUpdate);

    connect(client, &Discord::Client::messageCreated, this, &ClientInstance::onMessageCreated);

    connect(readStateManager, &ReadStateManager::readStateUpdated, this,
            &ClientInstance::readStateChanged);
    connect(readStateManager, &ReadStateManager::guildSettingsUpdated, this,
            &ClientInstance::guildSettingsChanged);

    connect(readStateManager, &ReadStateManager::ackRequested, this,
            &ClientInstance::handleAckRequest);
    connect(readStateManager, &ReadStateManager::bulkAckRequested, this,
            &ClientInstance::handleBulkAckRequest);

    connect(client, &Discord::Client::voiceStateUpdated, this,
            [this](const Discord::VoiceState &event) {
                qCInfo(LogVoice) << "VOICE_STATE_UPDATE: user" << event.userId.get()
                                 << "channel" << (event.channelId.isNull() ? 0 : static_cast<quint64>(event.channelId.get()))
                                 << "session" << event.sessionId.get()
                                 << "mute" << event.selfMute.get()
                                 << "deaf" << event.selfDeaf.get();

                // track our own voice state
                if (event.userId.get() == account.id) {
                    if (event.channelId.isNull() || !event.channelId.get().isValid()) {
                        currentVoiceChannelId = Snowflake::Invalid;
                        currentVoiceGuildId = Snowflake::Invalid;
                    } else {
                        currentVoiceChannelId = event.channelId.get();
                        currentVoiceGuildId = event.guildId.hasValue() ? event.guildId.get() : Snowflake::Invalid;
                    }
                    emit voiceStateChanged(currentVoiceChannelId, currentVoiceGuildId);
                }

                voiceManager->handleVoiceStateUpdate(event);
            });

    connect(client, &Discord::Client::voiceServerUpdated, this,
            [this](const Discord::VoiceServerUpdate &event) {
                qCInfo(LogVoice) << "VOICE_SERVER_UPDATE: guild" << event.guildId.get()
                                 << "endpoint" << (event.endpoint.isNull() ? "null" : event.endpoint.get())
                                 << "token" << event.token.get().left(8) + "...";

                voiceManager->handleVoiceServerUpdate(event);
            });
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

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        QList<Core::Snowflake> recipientIds;
        if (channel.recipients.hasValue()) {
            for (const auto &user : channel.recipients.get()) {
                userManager->saveUser(user);
                recipientIds.append(user.id.get());
            }
        } else if (channel.recipientIds.hasValue()) {
            recipientIds = channel.recipientIds.get();
        }

        if (!recipientIds.isEmpty())
            channelRepo.saveChannelRecipients(channelId, recipientIds, db);
    }

    db.commit();

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        Snowflake lastMsg = channel.lastMessageId.hasValue()
                                    ? channel.lastMessageId.get()
                                    : channelId;
        readStateManager->updateChannelLastMessageId(channelId, lastMsg);
    } else if (channel.lastMessageId.hasValue()) {
        readStateManager->updateChannelLastMessageId(channelId, channel.lastMessageId.get());
    }

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

void ClientInstance::onGuildRoleCreated(const Discord::GuildRoleCreate &event)
{
    if (!event.guildId.hasValue() || !event.role.hasValue()) {
        qCWarning(LogCore) << "Invalid GUILD_ROLE_CREATE event: missing guildId or role";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    const Discord::Role &role = event.role.get();

    if (!role.id.hasValue()) {
        qCWarning(LogCore) << "Invalid role in GUILD_ROLE_CREATE: missing role ID";
        return;
    }

    qCDebug(LogCore) << "Role created:" << role.id.get() << "in guild:" << guildId;

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    if (!db.transaction()) {
        qCWarning(LogCore) << "Failed to start transaction for role creation";
        return;
    }

    roleRepo.saveRole(guildId, role, db);

    if (!db.commit()) {
        qCWarning(LogCore) << "Failed to commit role creation:" << db.lastError().text();
        db.rollback();
        return;
    }

    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleCreated(guildId, role);
    emit guildRoleCreated(event);
}

void ClientInstance::onGuildRoleUpdated(const Discord::GuildRoleUpdate &event)
{
    if (!event.guildId.hasValue() || !event.role.hasValue()) {
        qCWarning(LogCore) << "Invalid GUILD_ROLE_UPDATE event: missing guildId or role";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    const Discord::Role &role = event.role.get();

    if (!role.id.hasValue()) {
        qCWarning(LogCore) << "Invalid role in GUILD_ROLE_UPDATE: missing role ID";
        return;
    }

    qCDebug(LogCore) << "Role updated:" << role.id.get() << "in guild:" << guildId;

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    if (!db.transaction()) {
        qCWarning(LogCore) << "Failed to start transaction for role update";
        return;
    }

    roleRepo.saveRole(guildId, role, db);

    if (!db.commit()) {
        qCWarning(LogCore) << "Failed to commit role update:" << db.lastError().text();
        db.rollback();
        return;
    }

    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleUpdated(guildId, role);
    emit guildRoleUpdated(event);
}

void ClientInstance::onGuildRoleDeleted(const Discord::GuildRoleDelete &event)
{
    if (!event.guildId.hasValue() || !event.roleId.hasValue()) {
        qCWarning(LogCore) << "Invalid GUILD_ROLE_DELETE event: missing guildId or roleId";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    Core::Snowflake roleId = event.roleId.get();

    qCDebug(LogCore) << "Role deleted:" << roleId << "in guild:" << guildId;

    QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
    QSqlDatabase db = QSqlDatabase::database(connName);

    if (!db.transaction()) {
        qCWarning(LogCore) << "Failed to start transaction for role deletion";
        return;
    }

    roleRepo.deleteRole(guildId, roleId, db);

    if (!db.commit()) {
        qCWarning(LogCore) << "Failed to commit role deletion:" << db.lastError().text();
        db.rollback();
        return;
    }

    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleDeleted(guildId, roleId);
    emit guildRoleDeleted(event);
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

void ClientInstance::onGuildMemberUpdate(const Discord::GuildMemberUpdate &event)
{
    Snowflake guildId = event.guildId.get();
    const auto &member = event.member.get();

    Snowflake userId = member.user.hasValue() ? member.user->id.get() : Snowflake::Invalid;
    if (!userId.isValid())
        return;

    if (member.user.hasValue())
        userManager->saveUser(member.user.get());
    userManager->saveMember(guildId, userId, member);

    if (userId == account.id)
        permissionManager->invalidateUserGuildCache(userId, guildId);

    emit membersUpdated(guildId, { userId });
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

void ClientInstance::onMessageCreated(const Discord::Message &msg)
{
    if (!msg.channelId.hasValue() || !msg.id.hasValue())
        return;

    Snowflake channelId = msg.channelId.get();
    Snowflake messageId = msg.id.get();

    bool isMention = isMessageMentioningMe(msg);
    readStateManager->handleMessageCreated(channelId, messageId, isMention);

    emit channelLastMessageUpdated(channelId, messageId);
}

bool ClientInstance::isMessageMentioningMe(const Discord::Message &msg) const
{
    if (msg.mentions.hasValue()) {
        for (const auto &user : msg.mentions.get())
            if (user.id.get() == account.id)
                return true;
    }

    if (msg.mentionRoles.hasValue() && msg.guildId.hasValue()) {
        Snowflake guildId = msg.guildId.get();
        Discord::Member *me = userManager->getMember(guildId, account.id);
        if (me && me->roles.hasValue()) {
            const auto &myRoles = me->roles.get();
            for (const auto &roleId : msg.mentionRoles.get())
                if (myRoles.contains(roleId))
                    return true;
        }
    }

    return false;
}

void ClientInstance::handleAckRequest(Snowflake channelId, Snowflake messageId)
{
    auto channelOpt = channelRepo.getChannel(channelId);
    bool isGuildChannel = channelOpt && channelOpt->guildId.hasValue();
    int ackFlags = isGuildChannel
                           ? static_cast<int>(Discord::ReadStateFlag::IS_GUILD_CHANNEL)
                           : 0;
    int lastViewed = ReadStateManager::daysSinceDiscordEpoch();
    client->ackMessage(channelId, messageId, ackFlags, lastViewed);
}

void ClientInstance::handleBulkAckRequest(const QList<QPair<Snowflake, Snowflake>> &pairs)
{
    QList<Discord::Client::AckEntry> entries;
    entries.reserve(pairs.size());
    for (const auto &[channelId, messageId] : pairs)
        entries.append({ channelId, messageId, 0 });

    constexpr int maxPerRequest = 100;
    int chunk = 0;
    for (int i = 0; i < entries.size(); i += maxPerRequest) {
        auto batch = entries.mid(i, maxPerRequest);
        if (chunk == 0) {
            client->ackBulk(batch);
        } else {
            QTimer::singleShot(chunk * 1000, this, [this, batch]() {
                client->ackBulk(batch);
            });
        }
        ++chunk;
    }
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
    if (isInVoice()) {
        discord()->sendVoiceStateUpdate(currentVoiceGuildId, Snowflake::Invalid, false, false);
        voiceManager->disconnect();
    }
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

ReadStateManager *ClientInstance::readState() const
{
    return readStateManager;
}

MemberListManager *ClientInstance::memberList() const
{
    return memberListManager;
}

AV::VoiceManager *ClientInstance::voice() const
{
    return voiceManager;
}

QList<Discord::Role> ClientInstance::getRolesForGuild(Snowflake guildId)
{
    return roleRepo.getRolesForGuild(guildId);
}

int ClientInstance::getChannelRateLimit(Snowflake channelId)
{
    auto channelOpt = channelRepo.getChannel(channelId);
    if (!channelOpt || !channelOpt->rateLimitPerUser.hasValue())
        return 0;
    return channelOpt->rateLimitPerUser.get();
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

Snowflake ClientInstance::voiceChannelId() const
{
    return currentVoiceChannelId;
}

Snowflake ClientInstance::voiceGuildId() const
{
    return currentVoiceGuildId;
}

bool ClientInstance::isInVoice() const
{
    return currentVoiceChannelId.isValid();
}

} // namespace Core
} // namespace Acheron
