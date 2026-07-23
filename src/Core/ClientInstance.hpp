#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class QSqlDatabase;

#include "AccountInfo.hpp"
#include "Discord/Client.hpp"
#include "MessageManager.hpp"
#include "MemberListManager.hpp"
#include "RelationshipManager.hpp"
#include "UserManager.hpp"
#include "PermissionManager.hpp"

namespace Acheron {
namespace Core {

#ifndef ACHERON_NO_VOICE
namespace AV {
class VoiceManager;
}
#endif

class ReadStateManager;
class ForumManager;

class ClientInstance : public QObject
{
    Q_OBJECT
public:
    explicit ClientInstance(const AccountInfo &info,
                            Discord::CaptchaResolver *captchaResolver = nullptr,
                            QObject *parent = nullptr);
    ~ClientInstance() override;

    void start();
    void stop();

    [[nodiscard]] Discord::Client *discord() const;
    [[nodiscard]] MessageManager *messages() const;
    [[nodiscard]] ForumManager *forums() const;
    [[nodiscard]] UserManager *users() const;
    [[nodiscard]] PermissionManager *permissions() const;
    [[nodiscard]] ReadStateManager *readState() const;
    [[nodiscard]] MemberListManager *memberList() const;
    [[nodiscard]] RelationshipManager *relationships() const;
#ifndef ACHERON_NO_VOICE
    [[nodiscard]] AV::VoiceManager *voice() const;
#endif

    [[nodiscard]] QList<Discord::Role> getRolesForGuild(Snowflake guildId);
    [[nodiscard]] QList<Discord::Role> getMemberRolesSorted(Snowflake guildId, Snowflake userId);
    [[nodiscard]] std::optional<Discord::Guild> getGuild(Snowflake guildId);
    [[nodiscard]] std::optional<Discord::Channel> getChannel(Snowflake channelId);
    [[nodiscard]] std::optional<Snowflake> findDmChannelWithUser(Snowflake userId);
    [[nodiscard]] int getChannelRateLimit(Snowflake channelId);

    [[nodiscard]] bool isThreadJoined(Snowflake threadId) const;

    struct ThreadListPage
    {
        QList<Discord::Channel> threads;
        bool hasMore = false;
    };
    using ThreadListPageCallback = std::function<void(const Result<ThreadListPage> &)>;
    void fetchThreadList(Snowflake channelId, bool archived, int offset, ThreadListPageCallback callback);

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
    void guildCreated(const Discord::GatewayGuild &guild);
    void guildRemoved(Snowflake guildId);
    void channelCreated(const Discord::ChannelCreate &event);
    void channelUpdated(const Discord::ChannelUpdate &update);
    void channelDeleted(const Discord::ChannelDelete &event);
    void threadCreated(const Discord::Channel &thread);
    void threadUpdated(const Discord::Channel &thread);
    void threadDeleted(Snowflake threadId, Snowflake parentId, Snowflake guildId);
    void threadListSynced(Snowflake guildId, const QList<Snowflake> &parentIds, const QList<Discord::Channel> &threads);
    void threadMembershipChanged(Snowflake threadId);
    void guildRoleCreated(const Discord::GuildRoleCreate &event);
    void guildRoleUpdated(const Discord::GuildRoleUpdate &event);
    void guildRoleDeleted(const Discord::GuildRoleDelete &event);
    void membersUpdated(Snowflake guildId, const QList<Snowflake> &userIds);
    void readStateChanged(Snowflake channelId);
    void forumBadgeChanged(Snowflake forumId);
    void forumJoinedPostsChanged(Snowflake forumId);
    void guildSettingsChanged(Snowflake guildId);
    void channelLastMessageUpdated(Snowflake channelId, Snowflake messageId);
    void voiceStateChanged(Snowflake channelId, Snowflake guildId);
    void authenticationFailed(const AccountInfo &info);

private slots:
    void onGuildCreated(const Discord::GatewayGuild &guild);
    void onGuildDeleted(const Discord::GuildDelete &event);
    void onChannelCreated(const Discord::ChannelCreate &event);
    void onChannelUpdated(const Discord::ChannelUpdate &event);
    void onChannelDeleted(const Discord::ChannelDelete &event);
    void onThreadCreated(const Discord::ChannelCreate &event);
    void onThreadUpdated(const Discord::ChannelUpdate &event);
    void onThreadDeleted(const Discord::ThreadDelete &event);
    void onThreadListSync(const Discord::ThreadListSync &event);
    void onThreadMemberUpdate(const Discord::ThreadMemberUpdate &event);
    void onThreadMembersUpdate(const Discord::ThreadMembersUpdate &event);
    void onGuildRoleCreated(const Discord::GuildRoleCreate &event);
    void onGuildRoleUpdated(const Discord::GuildRoleUpdate &event);
    void onGuildRoleDeleted(const Discord::GuildRoleDelete &event);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);
    void onGuildMemberUpdate(const Discord::GuildMemberUpdate &event);
    void onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update);
    void onMessagesReceived(const MessageRequestResult &result);
    void onMessageCreated(const Discord::Message &msg);
    void handleAckRequest(Snowflake channelId, Snowflake messageId);
    void handleBulkAckRequest(const QList<QPair<Snowflake, Snowflake>> &pairs);
    bool isMessageMentioningMe(const Discord::Message &msg) const;

private:
    bool runInCacheTransaction(const char *what, const std::function<void(QSqlDatabase &)> &op);

    void saveGuild(const Discord::GatewayGuild &guild, const QList<Discord::Member> *members, Snowflake myId, QSqlDatabase &db);
    void initGuildReadState(const Discord::GatewayGuild &guild);

    [[nodiscard]] bool isForumParent(Snowflake parentId);
    void registerThreadReadState(const Discord::Channel &thread, Snowflake guildId);
    void cacheThread(const Discord::Channel &thread, Snowflake guildId);
    void ingestThread(const Discord::Channel &thread, Snowflake guildId);
    void fetchThreadListAttempt(Snowflake channelId, bool archived, int offset, int attempt, const ThreadListPageCallback &callback);

    AccountInfo account;

    MessageManager *messageManager;
    ForumManager *forumManager;
    UserManager *userManager;
    Discord::Client *client;
    PermissionManager *permissionManager;
    ReadStateManager *readStateManager;
    MemberListManager *memberListManager;
    RelationshipManager *relationshipManager;
#ifndef ACHERON_NO_VOICE
    AV::VoiceManager *voiceManager;
#endif

    Storage::RoleRepository roleRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    QHash<Snowflake, QList<Discord::Role>> rolesCacheByGuild;

    Snowflake currentVoiceChannelId;
    Snowflake currentVoiceGuildId;

    // notFound members are kept in here so we dont ask for them again
    QSet<QPair<Snowflake /*guildId*/, Snowflake /*userId*/>> pendingMemberRequests;

    QSet<Snowflake> joinedThreads;
    QHash<Snowflake, Discord::Channel> threadCache;
    QHash<Snowflake, bool> forumParentCache;
};

} // namespace Core
} // namespace Acheron
