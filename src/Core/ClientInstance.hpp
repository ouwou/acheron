#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "AccountInfo.hpp"
#include "Discord/Client.hpp"
#include "MessageManager.hpp"
#include "MemberListManager.hpp"
#include "UserManager.hpp"
#include "PermissionManager.hpp"

namespace Acheron {
namespace Core {

namespace AV {
class VoiceManager;
}

class ReadStateManager;

class ClientInstance : public QObject
{
    Q_OBJECT
public:
    explicit ClientInstance(const AccountInfo &info, QObject *parent = nullptr);
    ~ClientInstance() override;

    void start();
    void stop();

    [[nodiscard]] Discord::Client *discord() const;
    [[nodiscard]] MessageManager *messages() const;
    [[nodiscard]] UserManager *users() const;
    [[nodiscard]] PermissionManager *permissions() const;
    [[nodiscard]] ReadStateManager *readState() const;
    [[nodiscard]] MemberListManager *memberList() const;
    [[nodiscard]] AV::VoiceManager *voice() const;

    [[nodiscard]] QList<Discord::Role> getRolesForGuild(Snowflake guildId);
    [[nodiscard]] int getChannelRateLimit(Snowflake channelId);

    [[nodiscard]] ConnectionState state() const;

    [[nodiscard]] Snowflake voiceChannelId() const;
    [[nodiscard]] Snowflake voiceGuildId() const;
    [[nodiscard]] bool isInVoice() const;

    Snowflake accountId() const;
    const AccountInfo &accountInfo() const;

signals:
    void stateChanged(ConnectionState state);
    void reconnecting(int attempt, int maxAttempts);
    void detailsUpdated(const AccountInfo &info);

    void ready(const Discord::Ready &ready);
    void channelCreated(const Discord::ChannelCreate &event);
    void channelUpdated(const Discord::ChannelUpdate &update);
    void channelDeleted(const Discord::ChannelDelete &event);
    void guildRoleCreated(const Discord::GuildRoleCreate &event);
    void guildRoleUpdated(const Discord::GuildRoleUpdate &event);
    void guildRoleDeleted(const Discord::GuildRoleDelete &event);
    void membersUpdated(Snowflake guildId, const QList<Snowflake> &userIds);
    void readStateChanged(Snowflake channelId);
    void guildSettingsChanged(Snowflake guildId);
    void channelLastMessageUpdated(Snowflake channelId, Snowflake messageId);
    void voiceStateChanged(Snowflake channelId, Snowflake guildId);

private slots:
    void onChannelCreated(const Discord::ChannelCreate &event);
    void onChannelUpdated(const Discord::ChannelUpdate &event);
    void onChannelDeleted(const Discord::ChannelDelete &event);
    void onGuildRoleCreated(const Discord::GuildRoleCreate &event);
    void onGuildRoleUpdated(const Discord::GuildRoleUpdate &event);
    void onGuildRoleDeleted(const Discord::GuildRoleDelete &event);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);
    void onGuildMemberUpdate(const Discord::GuildMemberUpdate &event);
    void onMessagesReceived(const MessageRequestResult &result);
    void onMessageCreated(const Discord::Message &msg);
    void handleAckRequest(Snowflake channelId, Snowflake messageId);
    void handleBulkAckRequest(const QList<QPair<Snowflake, Snowflake>> &pairs);
    bool isMessageMentioningMe(const Discord::Message &msg) const;

private:
    AccountInfo account;

    MessageManager *messageManager;
    UserManager *userManager;
    Discord::Client *client;
    PermissionManager *permissionManager;
    ReadStateManager *readStateManager;
    MemberListManager *memberListManager;
    AV::VoiceManager *voiceManager;

    Storage::RoleRepository roleRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    Snowflake currentVoiceChannelId;
    Snowflake currentVoiceGuildId;

    // notFound members are kept in here so we dont ask for them again
    QSet<QPair<Snowflake /*guildId*/, Snowflake /*userId*/>> pendingMemberRequests;
};

} // namespace Core
} // namespace Acheron
