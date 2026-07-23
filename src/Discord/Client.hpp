#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

#include <optional>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Enums.hpp"
#include "Core/PendingAttachment.hpp"

#include "Proto/UserSettings.hpp"

#include "Gateway.hpp"
#include "HttpClient.hpp"
#include "ClientIdentity.hpp"

using Acheron::Core::Snowflake;

namespace Acheron {
namespace Discord {

class Client : public QObject
{
    Q_OBJECT
public:
    enum class MessageLoadType {
        Latest,
        History,
        Future,
        Jump,
        Created,
    };
    Q_ENUM(MessageLoadType)

    explicit Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
                    CaptchaResolver *captchaResolver = nullptr, QObject *parent = nullptr);
    ~Client() override;

    void start();
    void stop();

    [[nodiscard]] Core::ConnectionState getState() const;

    using MessagesCallback = std::function<void(const Core::Result<QList<Message>> &)>;
    void fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback);
    void fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                      MessagesCallback callback);

    using ProfileCallback = std::function<void(const Core::Result<UserProfile> &)>;
    void fetchUserProfile(Snowflake userId, Snowflake guildId, ProfileCallback callback);

    void setUserNote(Snowflake userId, const QString &note);

    struct ForumThreadSearchResult
    {
        QList<Channel> threads;
        QHash<Snowflake, Message> firstMessages; // (thread id, starter message)
        bool hasMore = false;
        bool indexNotReady = false; // 202
        int retryAfterSeconds = 0;
    };
    using ForumThreadsCallback = std::function<void(const Core::Result<ForumThreadSearchResult> &)>;
    // "last_message_time" / "creation_time"
    void searchForumThreads(Snowflake forumId, int offset, const QString &sortBy, ForumThreadsCallback callback);

    struct CreatedForumThread
    {
        Channel thread;
        std::optional<Message> starterMessage;
    };
    using ForumThreadCallback = std::function<void(const Core::Result<CreatedForumThread> &)>;
    void createForumThread(Snowflake forumId, const QString &name,
                           const QList<Snowflake> &appliedTags, const QString &content,
                           const QString &nonce,
                           const QList<Core::PendingAttachment> &attachments,
                           ForumThreadCallback callback);

    using ForumPostDataCallback = std::function<void(const Core::Result<QHash<Snowflake, Message>> &)>;
    void fetchForumPostData(Snowflake forumId, const QList<Snowflake> &threadIds, ForumPostDataCallback callback);

    void joinThread(Snowflake threadId);
    void leaveThread(Snowflake threadId);

    struct ThreadListResult
    {
        QList<Channel> threads;
        QList<ThreadMember> members;
        bool hasMore = false;
        bool indexNotReady = false; // 202
        int retryAfterSeconds = 0;
    };
    using ThreadListCallback = std::function<void(const Core::Result<ThreadListResult> &)>;
    void searchThreads(Snowflake channelId, bool archived, int offset, ThreadListCallback callback);

    void sendMessage(Snowflake channelId, const QString &content, const QString &nonce,
                     Snowflake replyToMessageId = Snowflake::Invalid,
                     const QList<Core::PendingAttachment> &attachments = {});
    bool cancelMessageSend(const QString &nonce);
    void editMessage(Snowflake channelId, Snowflake messageId, const QString &content);
    void deleteMessage(Snowflake channelId, Snowflake messageId);
    void pinMessage(Snowflake channelId, Snowflake messageId);
    void unpinMessage(Snowflake channelId, Snowflake messageId);

    void addReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                     bool isBurst = false);
    void removeReaction(Snowflake channelId, Snowflake messageId, const QString &emoji,
                        bool isBurst = false);

    struct AckEntry
    {
        Snowflake channelId;
        Snowflake messageId;
        int readStateType = 0;
    };

    void ackMessage(Snowflake channelId, Snowflake messageId, int flags, int lastViewed);
    void ackBulk(const QList<AckEntry> &entries);

    void sendVoiceStateUpdate(Snowflake guildId, Snowflake channelId, bool selfMute, bool selfDeaf);

    void leaveGuild(Snowflake guildId);

    void debugForceReconnect();

    void subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                 const QList<QPair<int, int>> &ranges);
    void ensureSubscriptionByChannel(Snowflake channelId);
    void requestForumUnreads(Snowflake forumId, const QList<QPair<Snowflake, Snowflake>> &threads);
    void requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds);

    [[nodiscard]] Snowflake getGuildIdForChannel(Snowflake channelId) const;

    [[nodiscard]] PremiumTier getGuildPremiumTier(Snowflake guildId) const;
    [[nodiscard]] qint64 getMaxUploadSize(Snowflake channelId) const;

    [[nodiscard]] const Proto::PreloadedUserSettings &getSettings() const;
    [[nodiscard]] const User &getMe() const;

signals:
    void stateChanged(Core::ConnectionState state);
    void ready(const Ready &data);
    void readySupplemental(const ReadySupplemental &data);
    void messageCreated(const Message &msg);
    void messageUpdated(const Message &msg);
    void messageDeleted(const MessageDelete &event);
    void typingStart(const TypingStart &event);
    void channelCreated(const ChannelCreate &event);
    void channelUpdated(const ChannelUpdate &event);
    void channelDeleted(const ChannelDelete &event);
    void threadCreated(const ChannelCreate &event);
    void threadUpdated(const ChannelUpdate &event);
    void threadDeleted(const ThreadDelete &event);
    void threadListSync(const ThreadListSync &event);
    void threadMemberUpdated(const ThreadMemberUpdate &event);
    void threadMembersUpdated(const ThreadMembersUpdate &event);
    void forumUnreads(const ForumUnreads &event);
    void guildCreated(const GatewayGuild &guild);
    void guildDeleted(const GuildDelete &event);
    void guildMembersChunk(const GuildMembersChunk &chunk);
    void guildMemberUpdated(const GuildMemberUpdate &event);
    void guildRoleCreated(const GuildRoleCreate &event);
    void guildRoleUpdated(const GuildRoleUpdate &event);
    void guildRoleDeleted(const GuildRoleDelete &event);
    void messageAcked(const MessageAck &event);
    void messageReactionAdd(const MessageReactionAdd &event);
    void messageReactionAddMany(const MessageReactionAddMany &event);
    void messageReactionRemove(const MessageReactionRemove &event);
    void messageReactionRemoveAll(const MessageReactionRemoveAll &event);
    void messageReactionRemoveEmoji(const MessageReactionRemoveEmoji &event);
    void userGuildSettingsUpdated(const UserGuildSettings &settings);
    void guildMemberListUpdate(const GuildMemberListUpdate &event);
    void voiceStateUpdated(const VoiceState &event);
    void voiceServerUpdated(const VoiceServerUpdate &event);
    void relationshipAdded(const Relationship &event);
    void relationshipUpdated(const RelationshipPartial &event);
    void relationshipRemoved(const RelationshipPartial &event);
    void userNoteUpdated(const UserNoteUpdate &event);
    void messageSendFailed(const QString &nonce, const QString &error);
    void guildLeaveFailed(Core::Snowflake guildId, const QString &error);
    void attachmentUploadProgress(const QString &nonce, int fileIndex, qint64 sent, qint64 total);

    void reconnecting(int attempt, int maxAttempts);
    void errorOccurred(const QString &errorStr);
    void authenticationFailed();

private slots:
    void onConnected();
    void onDisconnected(CloseCode code, const QString &reason);

    void onGatewayReady(const Ready &data);
    void onGatewayReadySupplemental(const ReadySupplemental &data);
    void onGatewayMessageCreate(const Message &msg);
    void onGatewayMessageUpdate(const Message &msg);
    void onGatewayMessageDelete(const MessageDelete &event);
    void onGatewayChannelCreate(const ChannelCreate &event);
    void onGatewayChannelUpdate(const ChannelUpdate &event);
    void onGatewayChannelDelete(const ChannelDelete &event);
    void onGatewayThreadCreate(const ChannelCreate &event);
    void onGatewayThreadUpdate(const ChannelUpdate &event);
    void onGatewayThreadDelete(const ThreadDelete &event);
    void onGatewayThreadListSync(const ThreadListSync &event);
    void onGatewayGuildCreate(const GatewayGuild &guild);
    void onGatewayGuildDelete(const GuildDelete &event);
    void onGatewayGuildRoleCreate(const GuildRoleCreate &event);
    void onGatewayGuildRoleUpdate(const GuildRoleUpdate &event);
    void onGatewayGuildRoleDelete(const GuildRoleDelete &event);

private:
    void indexGuildMappings(const GatewayGuild &guild);
    void removeGuildMappings(Snowflake guildId);

    struct UploadState
    {
        Snowflake channelId;
        QString nonce;
        QList<Core::PendingAttachment> attachments;
        QStringList uploadFilenames;
        QList<bool> uploaded;
        int remaining = 0;
        bool failed = false;
        std::shared_ptr<std::atomic<bool>> cancelFlag;

        std::function<void(const QJsonArray &attachmentsJson)> onUploaded;
        std::function<void(const QString &error)> onFailed;
    };

    void setState(Core::ConnectionState state);
    void uploadAttachmentsAndSend(const std::shared_ptr<UploadState> &state);
    void finishUpload(const std::shared_ptr<UploadState> &state);
    void failUpload(const std::shared_ptr<UploadState> &state, const QString &error);
    void postForumThread(Snowflake forumId, const QString &name,
                         const QList<Snowflake> &appliedTags,
                         const QJsonObject &message,
                         ForumThreadCallback callback);
    void cleanupUploadedSlots(const std::shared_ptr<UploadState> &state);
    void settleUpload(const std::shared_ptr<UploadState> &state);

private:
    Core::ConnectionState state = Core::ConnectionState::Disconnected;

    QString baseUrl;
    QString token;

    ClientIdentity identity;
    HttpClient *httpClient;
    Gateway *gateway;

    QHash<Snowflake, Snowflake> channelToGuild; // todo prob move this somewhere or just a cache
    QHash<Snowflake, PremiumTier> guildPremiumTiers;
    QSet<Snowflake> subscribedGuilds;
    QHash<QString, std::shared_ptr<UploadState>> activeUploads; // by nonce

    Proto::PreloadedUserSettings settings;
    User me;
};

} // namespace Discord
} // namespace Acheron