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
#include "Emoji/EmojiManager.hpp"
#include "Gifs/FavoriteGifs.hpp"
#include "MessageManager.hpp"
#include "MemberListManager.hpp"
#include "Presence/PresenceManager.hpp"
#include "RelationshipManager.hpp"
#include "RoleHierarchy.hpp"
#include "UserManager.hpp"
#include "PermissionManager.hpp"
#include "Storage/AccountRepository.hpp"

namespace Acheron {
namespace Core {

#ifndef ACHERON_NO_VOICE
namespace Audio {
class VoiceManager;
}
#endif

class ReadStateManager;
class ForumManager;
class CallStore;
class StreamManager;
class StreamPreviews;
class StreamWatcher;
class StreamBroadcaster;
class CameraSender;
struct ShareTerms;

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
    [[nodiscard]] PresenceManager *presences() const;
    [[nodiscard]] EmojiManager *emojis() const;
    [[nodiscard]] FavoriteGifs *favoriteGifs() const;
    [[nodiscard]] CallStore *calls() const;
    [[nodiscard]] StreamManager *streams() const;
    [[nodiscard]] StreamPreviews *streamPreviews() const;
#ifndef ACHERON_NO_VOICE
    [[nodiscard]] Audio::VoiceManager *voice() const;
#endif
#ifdef ACHERON_HAVE_VIDEO
    [[nodiscard]] StreamWatcher *streamWatcher() const;
    [[nodiscard]] StreamBroadcaster *streamBroadcaster() const;
    [[nodiscard]] CameraSender *cameraSender() const;
#endif
    // Unmuted and undeafened, with the camera on only if it is on in that channel already.
    void joinVoice(Snowflake guildId, Snowflake channelId);
    void leaveVoice();

    [[nodiscard]] QList<Discord::Role> getRolesForGuild(Snowflake guildId);
    [[nodiscard]] QList<Discord::Role> getMemberRolesSorted(Snowflake guildId, Snowflake userId);
    [[nodiscard]] std::optional<Discord::Guild> getGuild(Snowflake guildId);
    [[nodiscard]] std::optional<Discord::Channel> getChannel(Snowflake channelId);
    [[nodiscard]] QList<Discord::Channel> getGuildChannels(Snowflake guildId);
    [[nodiscard]] std::optional<Snowflake> findDmChannelWithUser(Snowflake userId);
    using DmChannelCallback = std::function<void(const Result<Snowflake> &)>;
    void openDmChannel(Snowflake userId, DmChannelCallback callback);
    [[nodiscard]] int getChannelRateLimit(Snowflake channelId);

    [[nodiscard]] bool isThreadJoined(Snowflake threadId) const;

    // The channel the user is looking at changed. For a DM or group DM the gateway is asked
    // about its call, once, as the official client does when a private channel is selected.
    void noteChannelSelected(Snowflake channelId);

    [[nodiscard]] QList<Discord::Ban> observedBans(Snowflake guildId) const;
    [[nodiscard]] QList<Discord::Invite> createdInvites(Snowflake guildId) const;
    [[nodiscard]] std::optional<RoleHierarchy> selfRoleHierarchy(Snowflake guildId);

    void markGuildsAsRead(const QList<Snowflake> &guildIds);
    void markCategoryAsRead(Snowflake guildId, Snowflake categoryId);

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
    void guildUpdated(const Discord::Guild &guild);
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
    void memberRemoved(Snowflake guildId, Snowflake userId);
    void readStateChanged(Snowflake channelId);
    void forumBadgeChanged(Snowflake forumId);
    void forumJoinedPostsChanged(Snowflake forumId);
    void guildSettingsChanged(Snowflake guildId);
    void channelLastMessageUpdated(Snowflake channelId, Snowflake messageId);
    void voiceStateChanged(Snowflake channelId, Snowflake guildId);
    void authenticationFailed(const AccountInfo &info);

private slots:
    void onGuildCreated(const Discord::GatewayGuild &guild);
    void onGuildUpdated(const Discord::Guild &guild);
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
    void onGuildMemberAdded(const Discord::GuildMemberAdd &event);
    void onGuildMemberUpdate(const Discord::GuildMemberUpdate &event);
    void onGuildMemberRemoved(const Discord::GuildMemberRemove &event);
    void onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update);
    void onGuildBanAdded(const Discord::GuildBanEvent &event);
    void onGuildBanRemoved(const Discord::GuildBanEvent &event);
    void onInviteCreated(const Discord::Invite &invite);
    void onInviteRevoked(const QString &code);
    void onMessagesReceived(const MessageRequestResult &result);
    void onMessageCreated(const Discord::Message &msg);
    void handleAckRequest(Snowflake channelId, Snowflake messageId);
    void handleBulkAckRequest(const QList<QPair<Snowflake, Snowflake>> &pairs);
    void onVoiceConnected();
    void onVoiceDisconnected();
    void onVoiceStateUpdated(const Discord::VoiceState &event);
    void onCallCreated(const Discord::CallCreate &event);
    void onCallDeleted(const Discord::CallDelete &event);
    void onHeartbeatSessionChanged(const Discord::HeartbeatSession &session);
    bool isMessageMentioningMe(const Discord::Message &msg) const;

private:
    bool runInCacheTransaction(const char *what, const std::function<void(QSqlDatabase &)> &op);
    QList<Markdown::ChannelLinkPart> channelLinkParts(const Markdown::ChannelLinkRef &ref);

    void saveGuild(const Discord::GatewayGuild &guild, const QList<Discord::Member> *members, Snowflake myId, QSqlDatabase &db);
    void initGuildReadState(const Discord::GatewayGuild &guild);

    [[nodiscard]] bool isForumParent(Snowflake parentId);
    void registerThreadReadState(const Discord::Channel &thread, Snowflake guildId);
    void cacheThread(const Discord::Channel &thread, Snowflake guildId);
    void ingestThread(const Discord::Channel &thread, Snowflake guildId);
    void fetchThreadListAttempt(Snowflake channelId, bool archived, int offset, int attempt, const ThreadListPageCallback &callback);
    [[nodiscard]] QList<Snowflake> markableChannelIds(Snowflake guildId, Snowflake categoryId);
    void sendNextBulkAckBatch();

    AccountInfo account;

    MessageManager *messageManager;
    ForumManager *forumManager;
    UserManager *userManager;
    Discord::Client *client;
    PermissionManager *permissionManager;
    ReadStateManager *readStateManager;
    MemberListManager *memberListManager;
    RelationshipManager *relationshipManager;
    PresenceManager *presenceManager;
    EmojiManager *emojiManager;
    FavoriteGifs *favoriteGifManager;
    CallStore *callStore;
    StreamManager *streamManager;
    StreamPreviews *streamPreviewCache;
#ifndef ACHERON_NO_VOICE
    Audio::VoiceManager *voiceManager;
#endif
#ifdef ACHERON_HAVE_VIDEO
    StreamWatcher *streamWatch = nullptr;
    StreamBroadcaster *streamBroadcast = nullptr;
    CameraSender *cameraSend = nullptr;
    // Whether the account may share its screen in the voice channel or call it is in, by the
    // official client's rule, and what it may share there. The camera goes by the same rule.
    [[nodiscard]] ShareTerms screenShareTerms();
#endif

    Storage::RoleRepository roleRepo;
    Storage::AccountRepository accountRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    QHash<Snowflake, QList<Discord::Role>> rolesCacheByGuild;

    Snowflake currentVoiceChannelId;
    Snowflake currentVoiceGuildId;

    // Empty until READY. Voice states and calls are tied to the gateway session they came from.
    QString gatewaySessionId;
    Snowflake selectedChannelId;
    void connectSelectedCall(bool force);

    // notFound members are kept in here so we dont ask for them again
    QSet<QPair<Snowflake /*guildId*/, Snowflake /*userId*/>> pendingMemberRequests;

    QSet<Snowflake> joinedThreads;
    QHash<Snowflake, Discord::Channel> threadCache;
    QHash<Snowflake, bool> forumParentCache;

    QHash<Snowflake, QList<Discord::Ban>> observedBansByGuild;
    QHash<Snowflake, QList<Discord::Invite>> createdInvitesByGuild;

    QList<Discord::Client::AckEntry> queuedBulkAcks;
    bool bulkAckInFlight = false;
};

} // namespace Core
} // namespace Acheron
