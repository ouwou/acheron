#pragma once

#include <QObject>
#include <QString>

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Backoff.hpp"
#include "Core/ProxyConfig.hpp"
#include "Enums.hpp"
#include "IngestThread.hpp"
#include "Inbound.hpp"
#include "Outbound.hpp"
#include "Events.hpp"
#include "StreamKey.hpp"

namespace Acheron {

namespace Proto {
struct PreloadedUserSettings;
}

namespace Discord {

class ClientIdentity;

class Gateway : public QObject
{
    Q_OBJECT
public:
    explicit Gateway(const QString &token, const QString &gatewayUrl, ClientIdentity &identity,
                     const Core::ProxyConfig &proxy, QObject *parent = nullptr);
    ~Gateway();

    void start();
    void stop();
    void hardStop();

    [[nodiscard]] bool isRunning() const { return running; }

    void subscribeToGuild(Core::Snowflake guildId, Core::Snowflake channelId, const QList<QPair<int, int>> &ranges);
    void setMemberUpdatesSubscription(Core::Snowflake guildId, bool subscribed);
    void requestGuildMembers(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds, bool presences = true);
    void queryGuildMembers(Core::Snowflake guildId, const QString &query, int limit, bool presences);
    void requestForumUnreads(Core::Snowflake guildId, Core::Snowflake forumId,
                             const QList<QPair<Core::Snowflake, Core::Snowflake>> &threads);
    void sendVoiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf, bool selfVideo);
    void sendCallConnect(Core::Snowflake channelId);
    void sendPresenceUpdate(const QString &status, const QString &customStatus, qint64 idleSinceMs = 0);
    void sendStreamCreate(const StreamKey &channel, const std::optional<QString> &preferredRegion);
    void sendStreamDelete(const QString &streamKey);
    void sendStreamWatch(const QString &streamKey);
    void sendStreamPing(const QString &streamKey);
    void sendStreamSetPaused(const QString &streamKey, bool paused);

    // for the qos heartbeat
    void setActiveState(bool focused, bool rtcConnected);
    void sendUpdateTimeSpentSessionId();

    // Debug: simulate a server RECONNECT opcode
    void debugForceReconnect();
    void debugForceReidentify();

signals:
    void connected();
    void disconnected(CloseCode code, const QString &reason);
    void reconnecting(int attempt);

    // The network loop has exited for good; no reconnection will follow.
    void finished();

    void gatewayHello();
    void gatewayReady(const Ready &data);
    void gatewayReadySupplemental(const ReadySupplemental &data);
    void gatewayMessageCreate(const Message &data);
    void gatewayMessageUpdate(const Message &data);
    void gatewayMessageDelete(const MessageDelete &data);
    void gatewayTypingStart(const TypingStart &data);
    void gatewayChannelCreate(const ChannelCreate &data);
    void gatewayChannelUpdate(const ChannelUpdate &data);
    void gatewayChannelDelete(const ChannelDelete &data);
    void gatewayChannelPinsUpdate(const ChannelPinsUpdate &data);
    void gatewayChannelPinsAck(const ChannelPinsAck &data);
    void gatewayThreadCreate(const ChannelCreate &data);
    void gatewayThreadUpdate(const ChannelUpdate &data);
    void gatewayThreadDelete(const ThreadDelete &data);
    void gatewayThreadListSync(const ThreadListSync &data);
    void gatewayThreadMemberUpdate(const ThreadMemberUpdate &data);
    void gatewayThreadMembersUpdate(const ThreadMembersUpdate &data);
    void gatewayForumUnreads(const ForumUnreads &data);
    void gatewayGuildCreate(const GatewayGuild &data);
    void gatewayGuildUpdate(const Guild &data);
    void gatewayGuildDelete(const GuildDelete &data);
    void gatewayGuildMembersChunk(const GuildMembersChunk &data);
    void gatewayGuildMemberAdd(const GuildMemberAdd &data);
    void gatewayGuildMemberUpdate(const GuildMemberUpdate &data);
    void gatewayGuildMemberRemove(const GuildMemberRemove &data);
    void gatewayGuildRoleCreate(const GuildRoleCreate &data);
    void gatewayGuildRoleUpdate(const GuildRoleUpdate &data);
    void gatewayGuildRoleDelete(const GuildRoleDelete &data);
    void gatewayGuildEmojisUpdate(const GuildEmojisUpdate &data);
    void gatewayGuildStickersUpdate(const GuildStickersUpdate &data);
    void gatewayGuildBanAdd(const GuildBanEvent &data);
    void gatewayGuildBanRemove(const GuildBanEvent &data);
    void gatewayGuildPruneUpdate(const GuildPruneUpdate &data);
    void gatewayMessageAck(const MessageAck &data);
    void gatewayMessageReactionAdd(const MessageReactionAdd &data);
    void gatewayMessageReactionAddMany(const MessageReactionAddMany &data);
    void gatewayMessageReactionRemove(const MessageReactionRemove &data);
    void gatewayMessageReactionRemoveAll(const MessageReactionRemoveAll &data);
    void gatewayMessageReactionRemoveEmoji(const MessageReactionRemoveEmoji &data);
    void gatewayUserGuildSettingsUpdate(const UserGuildSettings &data);
    void gatewayNotificationSettingsUpdate(const NotificationSettings &data);
    void gatewayGuildMemberListUpdate(const GuildMemberListUpdate &data);
    void gatewayVoiceStateUpdate(const VoiceState &data);
    void gatewayVoiceServerUpdate(const VoiceServerUpdate &data);
    void gatewayCallCreate(const CallCreate &data);
    void gatewayCallUpdate(const CallUpdate &data);
    void gatewayCallDelete(const CallDelete &data);
    void gatewayStreamCreate(const StreamCreate &data);
    void gatewayStreamServerUpdate(const StreamServerUpdate &data);
    void gatewayStreamUpdate(const StreamUpdate &data);
    void gatewayStreamDelete(const StreamDelete &data);
    void gatewayRelationshipAdd(const Relationship &data);
    void gatewayRelationshipUpdate(const RelationshipPartial &data);
    void gatewayRelationshipRemove(const RelationshipPartial &data);
    void gatewayUserNoteUpdate(const UserNoteUpdate &data);
    void gatewayUserSettingsProtoUpdate(const UserSettingsProtoUpdate &data);
    void gatewayPresenceUpdate(const Presence &data);
    void gatewayPresencesReplace(const QList<Presence> &data);
    void gatewaySessionsReplace(const QList<UserSession> &data);

private:
    void sendPayload(const Core::OrderedJson &obj);
    void sendPayload(const QByteArray &data);

    // this function is called by the network thread
    void onPayloadReceived(const QJsonObject &root);
    void handleDispatch(const Inbound &data);
    void handleReady(const Inbound &data);
    void handleReadySupplemental(const Inbound &data);
    void handleMessageCreate(const Inbound &data);
    void handleMessageUpdate(const Inbound &data);
    void handleMessageDelete(const Inbound &data);
    void handleMessageDeleteBulk(const Inbound &data);
    void handleTypingStart(const Inbound &data);
    void handleChannelCreate(const Inbound &data);
    void handleChannelUpdate(const Inbound &data);
    void handleChannelDelete(const Inbound &data);
    void handleThreadCreate(const Inbound &data);
    void handleThreadUpdate(const Inbound &data);
    void handleThreadDelete(const Inbound &data);
    void handleThreadListSync(const Inbound &data);
    void handleThreadMemberUpdate(const Inbound &data);
    void handleThreadMembersUpdate(const Inbound &data);
    void handleForumUnreads(const Inbound &data);
    void handleGuildCreate(const Inbound &data);
    void handleGuildUpdate(const Inbound &data);
    void handleGuildDelete(const Inbound &data);
    void handleGuildMembersChunk(const Inbound &data);
    void handleGuildMemberAdd(const Inbound &data);
    void handleGuildMemberUpdate(const Inbound &data);
    void handleGuildMemberRemove(const Inbound &data);
    void handleGuildRoleCreate(const Inbound &data);
    void handleGuildRoleUpdate(const Inbound &data);
    void handleGuildRoleDelete(const Inbound &data);
    void handleGuildEmojisUpdate(const Inbound &data);
    void handleGuildStickersUpdate(const Inbound &data);
    void handleGuildBanAdd(const Inbound &data);
    void handleGuildBanRemove(const Inbound &data);
    void handleGuildPruneUpdate(const Inbound &data);
    void handleMessageAck(const Inbound &data);
    void handleMessageReactionAdd(const Inbound &data);
    void handleMessageReactionAddMany(const Inbound &data);
    void handleMessageReactionRemove(const Inbound &data);
    void handleMessageReactionRemoveAll(const Inbound &data);
    void handleMessageReactionRemoveEmoji(const Inbound &data);
    void handleUserGuildSettingsUpdate(const Inbound &data);
    void handleNotificationSettingsUpdate(const Inbound &data);
    void handleGuildMemberListUpdate(const Inbound &data);
    void handleVoiceStateUpdate(const Inbound &data);
    void handleVoiceStateUpdateBatch(const Inbound &data);
    void handleVoiceServerUpdate(const Inbound &data);
    void handleCallCreate(const Inbound &data);
    void handleCallUpdate(const Inbound &data);
    void handleCallDelete(const Inbound &data);
    void handleStreamCreate(const Inbound &data);
    void handleStreamServerUpdate(const Inbound &data);
    void handleStreamUpdate(const Inbound &data);
    void handleStreamDelete(const Inbound &data);
    void handleRelationshipAdd(const Inbound &data);
    void handleRelationshipUpdate(const Inbound &data);
    void handleRelationshipRemove(const Inbound &data);
    void handleUserNoteUpdate(const Inbound &data);
    void handleUserSettingsProtoUpdate(const Inbound &data);
    void handlePresenceUpdate(const Inbound &data);
    void handlePresencesReplace(const Inbound &data);
    void handleSessionsReplace(const Inbound &data);
    void handleHello(const Inbound &data);
    void handleInvalidSession(bool resumable);
    void resumeOrIdentify();
    void identify();
    void resume();
    bool isFatalCloseCode(CloseCode code) const;
    void succeedBackoff();
    void forgetSession();
    [[nodiscard]] QString connectionUrl();

    enum class ConnectionEnd {
        Stopped,
        Fatal,
        Lost,
        Reconnect,
    };

    void networkLoop();
    void runConnection();
    ConnectionEnd readUntilClosed();
    void sendClose(uint16_t code);
    void stopHeartbeat();
    bool waitBeforeReconnect();
    void heartbeatLoop();
    bool stoppedWhileWaiting(std::chrono::milliseconds delay);
    void sendHeartbeat();
    QoSPayload consumeQoSPayload();

    // join network and heartbeat threads, destroy ingest thread
    void teardown();

private:
    QString token;
    QString gatewayUrl;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;

    std::atomic<bool> running;

    std::mutex curlMutex;
    CURL *curl = nullptr;

    QByteArray receiveBuffer;
    IngestThread *ingest = nullptr;

    std::atomic<bool> wantToClose{ false };
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout = std::chrono::milliseconds(1000);

    std::atomic<int> lastReceivedSequence = 0;

    std::atomic<int> heartbeatInterval = 0;
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::mutex heartbeatThreadMutex;
    std::thread heartbeatThread;
    std::atomic<bool> heartbeatStopping{ false };
    std::atomic<bool> heartbeatRequested{ false };
    // 0 before the first one
    std::atomic<qint64> lastHeartbeatAckMs{ 0 };

    std::mutex qosMutex;
    std::optional<QoSPayload> currentQoS;
    std::optional<QoSPayload> upcomingQoS;
    std::atomic<bool> sessionEstablished{ false };

    // main thread and network thread
    std::mutex sessionMutex;
    QString sessionId;
    QString resumeGatewayUrl;
    Backoff backoff{ 1000, 60000 };
    std::deque<std::chrono::steady_clock::time_point> identifyTimes;

    std::atomic<bool> heartbeatAckReceived{ true };
    std::atomic<bool> shouldReconnect{ false };
    std::atomic<bool> reconnectImmediately{ false };
    std::atomic<bool> helloReceived{ false };
};

} // namespace Discord
} // namespace Acheron
