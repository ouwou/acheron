#include "Gateway.hpp"

#include "Enums.hpp"
#include "Objects.hpp"
#include "Outbound.hpp"
#include "Inbound.hpp"
#include "Events.hpp"
#include "CurlUtils.hpp"
#include "ClientIdentity.hpp"

#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QUrl>

#include <algorithm>
#include <cstdlib>

namespace Acheron {
namespace Discord {

static size_t write_cb(char *b, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

// Lets curl_easy_perform be interrupted mid-connect; without it a stop() request
// is not seen until the handshake finishes or times out.
static int abort_when_closing(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<std::atomic<bool> *>(userdata)->load() ? 1 : 0;
}

Gateway::Gateway(const QString &token, const QString &gatewayUrl, ClientIdentity &identity,
                 const Core::ProxyConfig &proxy, QObject *parent)
    : QObject(parent), token(token), gatewayUrl(gatewayUrl), identity(identity), proxy(proxy), running(false)
{
}

Gateway::~Gateway()
{
    hardStop();
}

void Gateway::start()
{
    if (running) {
        qCWarning(LogDiscord) << "Attempt to start already running gateway";
        return;
    }

    teardown();

    wantToClose = false;
    shouldReconnect = false;
    reconnectImmediately = false;

    ingest = new IngestThread(this);
    connect(ingest, &IngestThread::payloadReceived, this, &Gateway::onPayloadReceived);

    ingest->start();

    running = true;
    networkThread = std::thread(&Gateway::networkLoop, this);
}

void Gateway::stop()
{
    wantToClose = true;
    closeTime = std::chrono::steady_clock::now();
}

void Gateway::hardStop()
{
    shouldReconnect = false;
    teardown();
}

void Gateway::teardown()
{
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    stopHeartbeat();

    delete ingest;
    ingest = nullptr;
}

void Gateway::subscribeToGuild(Core::Snowflake guildId, Core::Snowflake channelId, const QList<QPair<int, int>> &ranges)
{
    GuildSubscriptionsBulk data;
    GuildSubscriptionsBulk::SubscriptionData guild;
    guild.typing = true;
    guild.activities = true;
    guild.threads = true;
    guild.channels.insert(channelId, ranges);
    data.subscriptions.get().insert(guildId, guild);

    qCDebug(LogDiscord) << "Subscribing to channel" << channelId << "with ranges" << ranges;

    sendPayload(data.toJson());
}

void Gateway::setMemberUpdatesSubscription(Core::Snowflake guildId, bool subscribed)
{
    GuildSubscriptionsBulk data;
    GuildSubscriptionsBulk::SubscriptionData guild;
    guild.memberUpdates = subscribed;
    data.subscriptions.get().insert(guildId, guild);

    sendPayload(data.toJson());
}

void Gateway::sendPayload(const Core::OrderedJson &obj)
{
    sendPayload(obj.toBytes());
}

void Gateway::sendPayload(const QByteArray &data)
{
    CurlUtils::wsSend(curl, curlMutex, data.constData(), data.size(), CURLWS_TEXT, "gateway");
}

void Gateway::onPayloadReceived(const QJsonObject &root)
{
    qCDebug(LogDiscord) << "Received payload" << root;

    Inbound msg = Inbound::fromJson(root);

    if (msg.s.has_value())
        lastReceivedSequence = msg.s.value();

    switch (msg.opcode) {
    case OpCode::DISPATCH:
        handleDispatch(msg);
        break;
    case OpCode::HELLO:
        handleHello(msg);
        break;
    case OpCode::HEARTBEAT:
        heartbeatRequested = true;
        heartbeatCv.notify_all();
        break;
    case OpCode::HEARTBEAT_ACK:
        heartbeatAckReceived = true;
        lastHeartbeatAckMs = QDateTime::currentMSecsSinceEpoch();
        break;
    case OpCode::RECONNECT:
        qCInfo(LogDiscord) << "Server requested reconnect";
        reconnectImmediately = true;
        shouldReconnect = true;
        break;
    case OpCode::INVALID_SESSION:
        handleInvalidSession(msg.data.toBool());
        break;
    default:
        break;
    }
}

void Gateway::handleDispatch(const Inbound &data)
{
    QString t = data.t.value_or("");
    qCDebug(LogDiscord) << "Received dispatch event" << t;

    if (t == "RESUMED") {
        qCInfo(LogDiscord) << "Session resumed";
        succeedBackoff();
        return;
    }

    GatewayEvent event = parseGatewayEvent(t);

    switch (event) {
    case GatewayEvent::READY:
        handleReady(data);
        break;
    case GatewayEvent::READY_SUPPLEMENTAL:
        handleReadySupplemental(data);
        break;
    case GatewayEvent::MESSAGE_CREATE:
        handleMessageCreate(data);
        break;
    case GatewayEvent::MESSAGE_UPDATE:
        handleMessageUpdate(data);
        break;
    case GatewayEvent::MESSAGE_DELETE:
        handleMessageDelete(data);
        break;
    case GatewayEvent::MESSAGE_DELETE_BULK:
        handleMessageDeleteBulk(data);
        break;
    case GatewayEvent::TYPING_START:
        handleTypingStart(data);
        break;
    case GatewayEvent::CHANNEL_CREATE:
        handleChannelCreate(data);
        break;
    case GatewayEvent::CHANNEL_UPDATE:
        handleChannelUpdate(data);
        break;
    case GatewayEvent::CHANNEL_DELETE:
        handleChannelDelete(data);
        break;
    case GatewayEvent::CHANNEL_PINS_UPDATE:
        emit gatewayChannelPinsUpdate(data.getData<ChannelPinsUpdate>());
        break;
    case GatewayEvent::CHANNEL_PINS_ACK:
        emit gatewayChannelPinsAck(data.getData<ChannelPinsAck>());
        break;
    case GatewayEvent::THREAD_CREATE:
        handleThreadCreate(data);
        break;
    case GatewayEvent::THREAD_UPDATE:
        handleThreadUpdate(data);
        break;
    case GatewayEvent::THREAD_DELETE:
        handleThreadDelete(data);
        break;
    case GatewayEvent::THREAD_LIST_SYNC:
        handleThreadListSync(data);
        break;
    case GatewayEvent::THREAD_MEMBER_UPDATE:
        handleThreadMemberUpdate(data);
        break;
    case GatewayEvent::THREAD_MEMBERS_UPDATE:
        handleThreadMembersUpdate(data);
        break;
    case GatewayEvent::FORUM_UNREADS:
        handleForumUnreads(data);
        break;
    case GatewayEvent::GUILD_CREATE:
        handleGuildCreate(data);
        break;
    case GatewayEvent::GUILD_UPDATE:
        handleGuildUpdate(data);
        break;
    case GatewayEvent::GUILD_DELETE:
        handleGuildDelete(data);
        break;
    case GatewayEvent::GUILD_MEMBERS_CHUNK:
        handleGuildMembersChunk(data);
        break;
    case GatewayEvent::GUILD_MEMBER_ADD:
        handleGuildMemberAdd(data);
        break;
    case GatewayEvent::GUILD_MEMBER_UPDATE:
        handleGuildMemberUpdate(data);
        break;
    case GatewayEvent::GUILD_MEMBER_REMOVE:
        handleGuildMemberRemove(data);
        break;
    case GatewayEvent::GUILD_ROLE_CREATE:
        handleGuildRoleCreate(data);
        break;
    case GatewayEvent::GUILD_ROLE_UPDATE:
        handleGuildRoleUpdate(data);
        break;
    case GatewayEvent::GUILD_ROLE_DELETE:
        handleGuildRoleDelete(data);
        break;
    case GatewayEvent::GUILD_EMOJIS_UPDATE:
        handleGuildEmojisUpdate(data);
        break;
    case GatewayEvent::GUILD_STICKERS_UPDATE:
        handleGuildStickersUpdate(data);
        break;
    case GatewayEvent::GUILD_BAN_ADD:
        handleGuildBanAdd(data);
        break;
    case GatewayEvent::GUILD_BAN_REMOVE:
        handleGuildBanRemove(data);
        break;
    case GatewayEvent::GUILD_PRUNE_UPDATE:
        handleGuildPruneUpdate(data);
        break;
    case GatewayEvent::MESSAGE_ACK:
        handleMessageAck(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_ADD:
        handleMessageReactionAdd(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_ADD_MANY:
        handleMessageReactionAddMany(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE:
        handleMessageReactionRemove(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE_ALL:
        handleMessageReactionRemoveAll(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE_EMOJI:
        handleMessageReactionRemoveEmoji(data);
        break;
    case GatewayEvent::USER_GUILD_SETTINGS_UPDATE:
        handleUserGuildSettingsUpdate(data);
        break;
    case GatewayEvent::NOTIFICATION_SETTINGS_UPDATE:
        handleNotificationSettingsUpdate(data);
        break;
    case GatewayEvent::GUILD_MEMBER_LIST_UPDATE:
        handleGuildMemberListUpdate(data);
        break;
    case GatewayEvent::VOICE_STATE_UPDATE:
        handleVoiceStateUpdate(data);
        break;
    case GatewayEvent::VOICE_STATE_UPDATE_BATCH:
        handleVoiceStateUpdateBatch(data);
        break;
    case GatewayEvent::VOICE_SERVER_UPDATE:
        handleVoiceServerUpdate(data);
        break;
    case GatewayEvent::CALL_CREATE:
        handleCallCreate(data);
        break;
    case GatewayEvent::CALL_UPDATE:
        handleCallUpdate(data);
        break;
    case GatewayEvent::CALL_DELETE:
        handleCallDelete(data);
        break;
    case GatewayEvent::STREAM_CREATE:
        handleStreamCreate(data);
        break;
    case GatewayEvent::STREAM_SERVER_UPDATE:
        handleStreamServerUpdate(data);
        break;
    case GatewayEvent::STREAM_UPDATE:
        handleStreamUpdate(data);
        break;
    case GatewayEvent::STREAM_DELETE:
        handleStreamDelete(data);
        break;
    case GatewayEvent::RELATIONSHIP_ADD:
        handleRelationshipAdd(data);
        break;
    case GatewayEvent::RELATIONSHIP_UPDATE:
        handleRelationshipUpdate(data);
        break;
    case GatewayEvent::RELATIONSHIP_REMOVE:
        handleRelationshipRemove(data);
        break;
    case GatewayEvent::USER_NOTE_UPDATE:
        handleUserNoteUpdate(data);
        break;
    case GatewayEvent::USER_SETTINGS_PROTO_UPDATE:
        handleUserSettingsProtoUpdate(data);
        break;
    case GatewayEvent::PRESENCE_UPDATE:
        handlePresenceUpdate(data);
        break;
    case GatewayEvent::PRESENCES_REPLACE:
        handlePresencesReplace(data);
        break;
    case GatewayEvent::SESSIONS_REPLACE:
        handleSessionsReplace(data);
        break;
    case GatewayEvent::UNKNOWN:
        qCInfo(LogDiscord) << "Unknown gateway event: " << t;
        break;
    default:
        qCInfo(LogDiscord) << "Parsed but unhandled gateway event: " << t;
    }
}

void Gateway::handleReady(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received ready event";

    Ready msg = data.getData<Ready>();

    {
        std::lock_guard lock(sessionMutex);
        if (msg.sessionId.hasValue())
            sessionId = msg.sessionId.get();
        if (msg.resumeGatewayUrl.hasValue())
            resumeGatewayUrl = msg.resumeGatewayUrl.get();
    }
    succeedBackoff();
    sessionEstablished = true;

    emit gatewayReady(msg);

    sendUpdateTimeSpentSessionId();
}

void Gateway::handleReadySupplemental(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received ready supplemental event";

    ReadySupplemental msg = data.getData<ReadySupplemental>();
    succeedBackoff();

    emit gatewayReadySupplemental(msg);
}

void Gateway::handleMessageCreate(const Inbound &data)
{
    Message msg = data.getData<Message>();

    emit gatewayMessageCreate(msg);
}

void Gateway::handleMessageUpdate(const Inbound &data)
{
    Message msg = data.getData<Message>();

    emit gatewayMessageUpdate(msg);
}

void Gateway::handleMessageDelete(const Inbound &data)
{
    MessageDelete event = data.getData<MessageDelete>();

    emit gatewayMessageDelete(event);
}

void Gateway::handleMessageDeleteBulk(const Inbound &data)
{
    const QJsonObject obj = data.data.toObject();
    MessageDelete event = MessageDelete::fromJson(obj);
    for (const QJsonValue &id : obj.value("ids").toArray()) {
        event.id = Core::Snowflake(id.toString().toULongLong());
        emit gatewayMessageDelete(event);
    }
}

void Gateway::handleTypingStart(const Inbound &data)
{
    TypingStart event = data.getData<TypingStart>();

    emit gatewayTypingStart(event);
}

void Gateway::handleChannelCreate(const Inbound &data)
{
    ChannelCreate event = data.getData<ChannelCreate>();

    emit gatewayChannelCreate(event);
}

void Gateway::handleChannelUpdate(const Inbound &data)
{
    ChannelUpdate event = data.getData<ChannelUpdate>();

    emit gatewayChannelUpdate(event);
}

void Gateway::handleChannelDelete(const Inbound &data)
{
    ChannelDelete event = data.getData<ChannelDelete>();

    emit gatewayChannelDelete(event);
}

void Gateway::handleThreadCreate(const Inbound &data)
{
    ChannelCreate event = data.getData<ChannelCreate>();

    emit gatewayThreadCreate(event);
}

void Gateway::handleThreadUpdate(const Inbound &data)
{
    ChannelUpdate event = data.getData<ChannelUpdate>();

    emit gatewayThreadUpdate(event);
}

void Gateway::handleThreadDelete(const Inbound &data)
{
    ThreadDelete event = data.getData<ThreadDelete>();

    emit gatewayThreadDelete(event);
}

void Gateway::handleThreadListSync(const Inbound &data)
{
    ThreadListSync event = data.getData<ThreadListSync>();

    emit gatewayThreadListSync(event);
}

void Gateway::handleThreadMemberUpdate(const Inbound &data)
{
    ThreadMemberUpdate event = data.getData<ThreadMemberUpdate>();

    emit gatewayThreadMemberUpdate(event);
}

void Gateway::handleThreadMembersUpdate(const Inbound &data)
{
    ThreadMembersUpdate event = data.getData<ThreadMembersUpdate>();

    emit gatewayThreadMembersUpdate(event);
}

void Gateway::handleForumUnreads(const Inbound &data)
{
    ForumUnreads event = data.getData<ForumUnreads>();

    emit gatewayForumUnreads(event);
}

void Gateway::requestForumUnreads(Core::Snowflake guildId, Core::Snowflake forumId,
                                  const QList<QPair<Core::Snowflake, Core::Snowflake>> &threads)
{
    RequestForumUnreads request;
    request.guildId = guildId;
    request.channelId = forumId;
    request.threads = threads;

    sendPayload(request.toJson());
}

void Gateway::handleGuildCreate(const Inbound &data)
{
    GatewayGuild guild = data.getData<GatewayGuild>();

    emit gatewayGuildCreate(guild);
}

void Gateway::handleGuildUpdate(const Inbound &data)
{
    Guild guild = data.getData<Guild>();

    emit gatewayGuildUpdate(guild);
}

void Gateway::handleGuildMembersChunk(const Inbound &data)
{
    GuildMembersChunk chunk = data.getData<GuildMembersChunk>();

    emit gatewayGuildMembersChunk(chunk);
}

void Gateway::handleGuildMemberAdd(const Inbound &data)
{
    GuildMemberAdd event = data.getData<GuildMemberAdd>();

    emit gatewayGuildMemberAdd(event);
}

void Gateway::handleGuildMemberUpdate(const Inbound &data)
{
    GuildMemberUpdate event = data.getData<GuildMemberUpdate>();

    emit gatewayGuildMemberUpdate(event);
}

void Gateway::handleGuildMemberRemove(const Inbound &data)
{
    GuildMemberRemove event = data.getData<GuildMemberRemove>();

    emit gatewayGuildMemberRemove(event);
}

void Gateway::handleGuildRoleCreate(const Inbound &data)
{
    GuildRoleCreate event = data.getData<GuildRoleCreate>();

    emit gatewayGuildRoleCreate(event);
}

void Gateway::handleGuildRoleUpdate(const Inbound &data)
{
    GuildRoleUpdate event = data.getData<GuildRoleUpdate>();

    emit gatewayGuildRoleUpdate(event);
}

void Gateway::handleGuildRoleDelete(const Inbound &data)
{
    GuildRoleDelete event = data.getData<GuildRoleDelete>();

    emit gatewayGuildRoleDelete(event);
}

void Gateway::handleGuildEmojisUpdate(const Inbound &data)
{
    GuildEmojisUpdate event = data.getData<GuildEmojisUpdate>();

    emit gatewayGuildEmojisUpdate(event);
}

void Gateway::handleGuildStickersUpdate(const Inbound &data)
{
    GuildStickersUpdate event = data.getData<GuildStickersUpdate>();

    emit gatewayGuildStickersUpdate(event);
}

void Gateway::handleGuildBanAdd(const Inbound &data)
{
    GuildBanEvent event = data.getData<GuildBanEvent>();

    emit gatewayGuildBanAdd(event);
}

void Gateway::handleGuildBanRemove(const Inbound &data)
{
    GuildBanEvent event = data.getData<GuildBanEvent>();

    emit gatewayGuildBanRemove(event);
}

void Gateway::handleGuildPruneUpdate(const Inbound &data)
{
    GuildPruneUpdate event = data.getData<GuildPruneUpdate>();

    emit gatewayGuildPruneUpdate(event);
}

void Gateway::handleGuildDelete(const Inbound &data)
{
    GuildDelete event = data.getData<GuildDelete>();

    emit gatewayGuildDelete(event);
}

void Gateway::handleMessageAck(const Inbound &data)
{
    MessageAck event = data.getData<MessageAck>();

    emit gatewayMessageAck(event);
}

void Gateway::handleMessageReactionAdd(const Inbound &data)
{
    MessageReactionAdd event = data.getData<MessageReactionAdd>();

    emit gatewayMessageReactionAdd(event);
}

void Gateway::handleMessageReactionAddMany(const Inbound &data)
{
    MessageReactionAddMany event = data.getData<MessageReactionAddMany>();

    emit gatewayMessageReactionAddMany(event);
}

void Gateway::handleMessageReactionRemove(const Inbound &data)
{
    MessageReactionRemove event = data.getData<MessageReactionRemove>();

    emit gatewayMessageReactionRemove(event);
}

void Gateway::handleMessageReactionRemoveAll(const Inbound &data)
{
    MessageReactionRemoveAll event = data.getData<MessageReactionRemoveAll>();

    emit gatewayMessageReactionRemoveAll(event);
}

void Gateway::handleMessageReactionRemoveEmoji(const Inbound &data)
{
    MessageReactionRemoveEmoji event = data.getData<MessageReactionRemoveEmoji>();

    emit gatewayMessageReactionRemoveEmoji(event);
}

void Gateway::handleUserGuildSettingsUpdate(const Inbound &data)
{
    UserGuildSettings settings = data.getData<UserGuildSettings>();

    emit gatewayUserGuildSettingsUpdate(settings);
}

void Gateway::handleNotificationSettingsUpdate(const Inbound &data)
{
    NotificationSettings settings = data.getData<NotificationSettings>();

    emit gatewayNotificationSettingsUpdate(settings);
}

void Gateway::handleGuildMemberListUpdate(const Inbound &data)
{
    GuildMemberListUpdate update = data.getData<GuildMemberListUpdate>();

    emit gatewayGuildMemberListUpdate(update);
}

void Gateway::handleVoiceStateUpdate(const Inbound &data)
{
    VoiceState event = data.getData<VoiceState>();

    emit gatewayVoiceStateUpdate(event);
}

void Gateway::handleVoiceStateUpdateBatch(const Inbound &data)
{
    VoiceStateUpdateBatch batch = data.getData<VoiceStateUpdateBatch>();
    if (!batch.voiceStates.hasValue())
        return;

    for (const VoiceState &state : batch.voiceStates.get())
        emit gatewayVoiceStateUpdate(state);
}

void Gateway::handleVoiceServerUpdate(const Inbound &data)
{
    VoiceServerUpdate event = data.getData<VoiceServerUpdate>();

    emit gatewayVoiceServerUpdate(event);
}

void Gateway::handleCallCreate(const Inbound &data)
{
    emit gatewayCallCreate(data.getData<CallCreate>());
}

void Gateway::handleCallUpdate(const Inbound &data)
{
    emit gatewayCallUpdate(data.getData<CallUpdate>());
}

void Gateway::handleCallDelete(const Inbound &data)
{
    emit gatewayCallDelete(data.getData<CallDelete>());
}

void Gateway::handleStreamCreate(const Inbound &data)
{
    emit gatewayStreamCreate(data.getData<StreamCreate>());
}

void Gateway::handleStreamServerUpdate(const Inbound &data)
{
    emit gatewayStreamServerUpdate(data.getData<StreamServerUpdate>());
}

void Gateway::handleStreamUpdate(const Inbound &data)
{
    emit gatewayStreamUpdate(data.getData<StreamUpdate>());
}

void Gateway::handleStreamDelete(const Inbound &data)
{
    emit gatewayStreamDelete(data.getData<StreamDelete>());
}

void Gateway::handleRelationshipAdd(const Inbound &data)
{
    Relationship event = data.getData<Relationship>();
    emit gatewayRelationshipAdd(event);
}

void Gateway::handleRelationshipUpdate(const Inbound &data)
{
    RelationshipPartial event = data.getData<RelationshipPartial>();
    emit gatewayRelationshipUpdate(event);
}

void Gateway::handleRelationshipRemove(const Inbound &data)
{
    RelationshipPartial event = data.getData<RelationshipPartial>();
    emit gatewayRelationshipRemove(event);
}

void Gateway::handleUserNoteUpdate(const Inbound &data)
{
    UserNoteUpdate event = data.getData<UserNoteUpdate>();
    emit gatewayUserNoteUpdate(event);
}

void Gateway::handleUserSettingsProtoUpdate(const Inbound &data)
{
    UserSettingsProtoUpdate event = data.getData<UserSettingsProtoUpdate>();

    emit gatewayUserSettingsProtoUpdate(event);
}

void Gateway::handlePresenceUpdate(const Inbound &data)
{
    Presence event = data.getData<Presence>();
    emit gatewayPresenceUpdate(event);
}

void Gateway::handlePresencesReplace(const Inbound &data)
{
    const QJsonArray array = data.data.toArray();

    QList<Presence> presences;
    presences.reserve(array.size());
    for (const QJsonValue &value : array)
        presences.append(Presence::fromJson(value.toObject()));

    qCDebug(LogDiscord) << "PRESENCES_REPLACE:" << presences.size() << "presences";
    emit gatewayPresencesReplace(presences);
}

void Gateway::handleSessionsReplace(const Inbound &data)
{
    const QJsonArray array = data.data.toArray();

    QList<UserSession> sessions;
    sessions.reserve(array.size());
    for (const QJsonValue &value : array)
        sessions.append(UserSession::fromJson(value.toObject()));

    qCDebug(LogDiscord) << "SESSIONS_REPLACE:" << sessions.size() << "sessions";
    emit gatewaySessionsReplace(sessions);
}

void Gateway::requestGuildMembers(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds, bool presences)
{
    for (qsizetype start = 0; start < userIds.size(); start += 100) {
        RequestGuildMembers request;
        request.guildId = guildId;
        request.userIds = userIds.mid(start, 100);
        request.presences = presences;
        sendPayload(request.toJson());
    }
}

void Gateway::queryGuildMembers(Core::Snowflake guildId, const QString &query, int limit, bool presences)
{
    RequestGuildMembers request;
    request.guildId = guildId;
    request.query = query;
    request.limit = limit;
    request.presences = presences;

    sendPayload(request.toJson());
}

void Gateway::sendVoiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf, bool selfVideo)
{
    sendPayload(voiceStateUpdate(guildId, channelId, selfMute, selfDeaf, selfVideo).toJson());
}

void Gateway::sendCallConnect(Core::Snowflake channelId)
{
    ConnectCall msg;
    msg.channelId = channelId;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamCreate(const StreamKey &channel, const std::optional<QString> &preferredRegion)
{
    CreateStream msg;
    msg.type = channel.typeName();
    if (channel.type == StreamKey::Type::Guild)
        msg.guildId = channel.guildId;
    else
        msg.guildId = nullptr;
    msg.channelId = channel.channelId;
    if (preferredRegion)
        msg.preferredRegion = *preferredRegion;
    else
        msg.preferredRegion = nullptr;

    sendPayload(msg.toJson());
}

void Gateway::sendStreamDelete(const QString &streamKey)
{
    DeleteStream msg;
    msg.streamKey = streamKey;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamWatch(const QString &streamKey)
{
    WatchStream msg;
    msg.streamKey = streamKey;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamPing(const QString &streamKey)
{
    PingStream msg;
    msg.streamKey = streamKey;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamSetPaused(const QString &streamKey, bool paused)
{
    SetStreamPaused msg;
    msg.streamKey = streamKey;
    msg.paused = paused;
    sendPayload(msg.toJson());
}

void Gateway::handleHello(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received hello";

    Hello msg = data.getData<Hello>();

    heartbeatInterval = msg.heartbeatInterval;
    heartbeatAckReceived = true;
    helloReceived = true;

    resumeOrIdentify();

    {
        std::lock_guard lock(heartbeatThreadMutex);
        if (!heartbeatThread.joinable()) {
            heartbeatStopping = false;
            heartbeatThread = std::thread(&Gateway::heartbeatLoop, this);
        }
    }

    emit gatewayHello();
}

void Gateway::handleInvalidSession(bool resumable)
{
    qCInfo(LogDiscord) << "Invalid session, resumable:" << resumable;
    // the official client answers on the same socket
    if (resumable)
        resumeOrIdentify();
    else
        identify();
}

void Gateway::resumeOrIdentify()
{
    // an ack older than this means the session is likely gone
    constexpr qint64 resumableAckAgeMs = 3 * 60 * 1000;

    bool hasSession;
    {
        std::lock_guard lock(sessionMutex);
        hasSession = !sessionId.isEmpty();
    }
    const qint64 lastAck = lastHeartbeatAckMs;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (hasSession && (lastAck == 0 || now - lastAck <= resumableAckAgeMs))
        resume();
    else
        identify();
    lastHeartbeatAckMs = now;
}

void Gateway::succeedBackoff()
{
    std::lock_guard lock(sessionMutex);
    backoff.succeed();
}

void Gateway::forgetSession()
{
    std::lock_guard lock(sessionMutex);
    sessionId.clear();
    resumeGatewayUrl.clear();
    lastReceivedSequence = 0;
}

void Gateway::identify()
{
    // stop instead of looking like a client stuck in a loop
    constexpr int maxIdentifiesPerHour = 10;

    const auto now = std::chrono::steady_clock::now();
    bool tooMany;
    {
        std::lock_guard lock(sessionMutex);
        while (!identifyTimes.empty() && now - identifyTimes.front() > std::chrono::hours(1))
            identifyTimes.pop_front();
        tooMany = identifyTimes.size() >= maxIdentifiesPerHour;
        if (!tooMany)
            identifyTimes.push_back(now);
        sessionId.clear();
    }
    if (tooMany) {
        qCWarning(LogDiscord) << "Identified" << maxIdentifiesPerHour << "times in the last hour, not reconnecting";
        emit disconnected(CloseCode::INTERNAL, "Too many new sessions in the last hour");
        stop();
        return;
    }
    lastReceivedSequence = 0;

    ClientPropertiesBuildParams params;
    params.includeClientHeartbeatSessionId = true;
    params.isFastConnect = false;
    params.gatewayConnectReasons = "AppSkeleton";
    ClientProperties properties = identity.buildClientProperties(params);

    UpdatePresence presence;
    presence.status = "unknown";
    presence.since = 0;
    presence.afk = false;

    ClientState clientState;

    Identify identify;
    identify.token = token;
    identify.capabilities = CURRENT_CAPABILITIES;
    identify.compress = false;
    identify.properties = properties;
    identify.presence = presence;
    identify.clientState = clientState;

    sendPayload(identify.toJson());
}

static int curlDebug(CURL *, curl_infotype type, char *data, size_t size, void *)
{
    if (type == CURLINFO_TEXT || type == CURLINFO_SSL_DATA_IN || type == CURLINFO_SSL_DATA_OUT) {
        qDebug().noquote() << QByteArray(data, size);
    }
    return 0;
}

void Gateway::networkLoop()
{
    runConnection();

    running = false;
    stopHeartbeat();

    emit finished();
}

QString Gateway::connectionUrl()
{
    std::lock_guard lock(sessionMutex);
    if (sessionId.isEmpty() || resumeGatewayUrl.isEmpty())
        return gatewayUrl;
    // READY's resume url is a bare host
    QUrl resumeUrl(resumeGatewayUrl);
    resumeUrl.setQuery(QUrl(gatewayUrl).query());
    return resumeUrl.toString();
}

void Gateway::runConnection()
{
    while (running && !wantToClose) {
        shouldReconnect = false;
        helloReceived = false;
        const QString connectUrl = connectionUrl();

        curl = curl_easy_init();
        if (!curl) {
            qCCritical(LogDiscord) << "Failed to initialize curl";
            emit disconnected(CloseCode::INTERNAL, "Failed to initialize curl");
            return;
        }

        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_closing);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &wantToClose);
        CurlUtils::applyCommonOptions(curl);
        CurlUtils::applyProxy(curl, proxy);

        const CURLcode res = CurlUtils::performWebSocketHandshake(curl, connectUrl.toUtf8());
        ConnectionEnd end;
        if (res != CURLE_OK) {
            if (wantToClose) {
                end = ConnectionEnd::Stopped;
                emit disconnected(CloseCode::CONNECTION_REQUEST_CANCELED, "Connection cancelled");
            } else {
                end = ConnectionEnd::Lost;
                qCWarning(LogDiscord) << "Failed to connect to gateway:" << curl_easy_strerror(res);
            }
        } else {
            emit connected();
            end = readUntilClosed();
        }

        sessionEstablished = false;
        stopHeartbeat();
        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
        // the next connection starts a fresh zlib context
        ingest->reset();

        if (end == ConnectionEnd::Stopped || end == ConnectionEnd::Fatal)
            return;
        if (!waitBeforeReconnect())
            return;
    }
}

Gateway::ConnectionEnd Gateway::readUntilClosed()
{
    constexpr auto helloTimeout = std::chrono::seconds(30);
    const auto connectedAt = std::chrono::steady_clock::now();

    char chunk[8192];
    bool closeSent = false;
    while (running) {
        if (!helloReceived && std::chrono::steady_clock::now() - connectedAt > helloTimeout) {
            qCWarning(LogDiscord) << "No HELLO in" << helloTimeout.count() << "seconds";
            shouldReconnect = true;
        }
        if (shouldReconnect) {
            sendClose(4000);
            return ConnectionEnd::Reconnect;
        }

        size_t rlen = 0;
        const curl_ws_frame *meta = nullptr;
        CURLcode res;
        {
            std::lock_guard lock(curlMutex);

            if (wantToClose) {
                if (!closeSent) {
                    closeSent = true;
                    uint8_t closePayload[2] = { 0x03, 0xE8 };
                    size_t bytesSent = 0;
                    curl_ws_send(curl, closePayload, sizeof(closePayload), &bytesSent, 0, CURLWS_CLOSE);
                }
                if (std::chrono::steady_clock::now() - closeTime > closeTimeout) {
                    qCDebug(LogDiscord) << "Gateway close timeout";
                    return ConnectionEnd::Stopped;
                }
            }

            res = curl_ws_recv(curl, chunk, sizeof(chunk), &rlen, &meta);
        }

        if (res == CURLE_AGAIN || res == CURLE_GOT_NOTHING) {
            CurlUtils::wsRecvWait(curl, curlMutex);
            continue;
        }

        if (res != CURLE_OK) {
            qCWarning(LogDiscord) << "curl_ws_recv failed:" << curl_easy_strerror(res);
            return wantToClose ? ConnectionEnd::Stopped : ConnectionEnd::Lost;
        }

        if (!meta)
            continue;

        if (meta->flags & CURLWS_CLOSE) {
            int closeCode = 1000;
            QString closeReason;
            if (rlen >= 2) {
                closeCode = (uint8_t(chunk[0]) << 8) | uint8_t(chunk[1]);
                if (rlen > 2)
                    closeReason = QString::fromUtf8(chunk + 2, rlen - 2);
            }

            qCInfo(LogDiscord) << "Connection closed with code" << closeCode << "reason:" << closeReason;
            const CloseCode cc = static_cast<CloseCode>(closeCode);
            emit disconnected(cc, closeReason);
            if (wantToClose)
                return ConnectionEnd::Stopped;
            return isFatalCloseCode(cc) ? ConnectionEnd::Fatal : ConnectionEnd::Lost;
        }

        if (meta->flags & (CURLWS_PING | CURLWS_PONG))
            continue;

        ingest->push(QByteArray(chunk, rlen));
    }
    return ConnectionEnd::Stopped;
}

void Gateway::sendClose(uint16_t code)
{
    const uint8_t payload[2] = { static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF) };
    size_t sent = 0;
    std::lock_guard lock(curlMutex);
    curl_ws_send(curl, payload, sizeof(payload), &sent, 0, CURLWS_CLOSE);
}

void Gateway::stopHeartbeat()
{
    std::lock_guard lock(heartbeatThreadMutex);
    heartbeatStopping = true;
    heartbeatCv.notify_all();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

bool Gateway::waitBeforeReconnect()
{
    int delayMs = 0;
    int attempt = 0;
    if (!reconnectImmediately.exchange(false)) {
        bool forget;
        {
            std::lock_guard lock(sessionMutex);
            delayMs = backoff.fail();
            attempt = backoff.failures();
            forget = attempt > 4;
        }
        if (forget)
            forgetSession();
    }

    qCInfo(LogDiscord) << "Reconnecting in" << delayMs << "ms, attempt" << attempt;
    emit reconnecting(attempt);

    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    while (running && !wantToClose && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return running && !wantToClose;
}

bool Gateway::stoppedWhileWaiting(std::chrono::milliseconds delay)
{
    std::unique_lock lock(heartbeatMutex);
    return heartbeatCv.wait_for(lock, delay, [this] {
        return !running || heartbeatStopping.load() || shouldReconnect.load() || heartbeatRequested.load();
    }) && !heartbeatRequested.load();
}

void Gateway::heartbeatLoop()
{
    const int interval = heartbeatInterval;
    qCDebug(LogDiscord) << "Heartbeat loop started, interval:" << interval;

    // the official client sends its first one at a random point in the interval
    const auto firstDelay = std::chrono::milliseconds(QRandomGenerator::global()->bounded(std::max(interval, 1)));
    if (stoppedWhileWaiting(firstDelay))
        return;
    heartbeatAckReceived = true;

    while (true) {
        if (heartbeatRequested.exchange(false)) {
            sendHeartbeat();
        } else {
            if (!heartbeatAckReceived) {
                qCWarning(LogDiscord) << "No heartbeat ACK received — zombie connection detected";
                shouldReconnect = true;
                return;
            }
            heartbeatAckReceived = false;
            sendHeartbeat();
        }

        if (stoppedWhileWaiting(std::chrono::milliseconds(interval)))
            return;
    }
}

void Gateway::sendHeartbeat()
{
    QoSHeartbeat heartbeat;
    heartbeat.seq = lastReceivedSequence;
    heartbeat.qos = consumeQoSPayload();
    sendPayload(heartbeat.toJson());
}

void Gateway::sendUpdateTimeSpentSessionId()
{
    if (!sessionEstablished)
        return;

    auto session = identity.heartbeatSession();

    if (!session)
        return;

    UpdateTimeSpentSessionId payload;
    payload.initializationTimestamp = session->createdAtMs;
    payload.sessionId = session->id;
    payload.clientLaunchId = identity.clientLaunchId();
    sendPayload(payload.toJson());
    sendHeartbeat();
}

static QoSPayload makeQoSPayload(const QList<QString> &reasons)
{
    QoSPayload payload;
    payload.ver = 30;
    payload.active = !reasons.isEmpty();
    payload.reasons = reasons;
    return payload;
}

QoSPayload Gateway::consumeQoSPayload()
{
    std::lock_guard lock(qosMutex);
    QoSPayload payload = currentQoS.value_or(makeQoSPayload({}));
    if (upcomingQoS.has_value())
        currentQoS = *upcomingQoS;
    upcomingQoS.reset();
    return payload;
}

void Gateway::setActiveState(bool focused, bool rtcConnected)
{
    QList<QString> reasons;
    if (focused)
        reasons.append("foregrounded");
    if (rtcConnected)
        reasons.append("rtc_connected");
    QoSPayload state = makeQoSPayload(reasons);

    // this is awkward but it mimics what discord does
    bool sendNow = false;
    {
        std::lock_guard lock(qosMutex);
        bool wasInactive = !currentQoS.has_value() || !currentQoS->active.get();
        if (!currentQoS.has_value())
            currentQoS = state;
        if (state.active.get()) {
            QList<QString> merged = currentQoS->reasons.get();
            for (const QString &reason : reasons)
                if (!merged.contains(reason))
                    merged.append(reason);
            std::sort(merged.begin(), merged.end());

            currentQoS->active = true;
            currentQoS->reasons = merged;
            sendNow = wasInactive && sessionEstablished;
        }
        upcomingQoS = state;
    }

    if (sendNow)
        sendHeartbeat();
}

void Gateway::debugForceReconnect()
{
    qCInfo(LogDiscord) << "DEBUG: Forcing reconnect (simulating op 7 RECONNECT)";
    reconnectImmediately = true;
    shouldReconnect = true;
}

void Gateway::debugForceReidentify()
{
    qCInfo(LogDiscord) << "DEBUG: Forcing a new session";
    forgetSession();
    reconnectImmediately = true;
    shouldReconnect = true;
}

void Gateway::resume()
{
    qCInfo(LogDiscord) << "Sending RESUME";
    Resume resumeMsg;
    resumeMsg.token = token;
    {
        std::lock_guard lock(sessionMutex);
        resumeMsg.sessionId = sessionId;
    }
    resumeMsg.seq = lastReceivedSequence.load();
    sendPayload(resumeMsg.toJson());
    sessionEstablished = true;
}

bool Gateway::isFatalCloseCode(CloseCode code) const
{
    switch (code) {
    case CloseCode::AUTHENTICATION_FAILED:
    case CloseCode::INVALID_SHARD:
    case CloseCode::SHARDING_REQUIRED:
    case CloseCode::INVALID_API_VERSION:
    case CloseCode::INVALID_INTENTS:
    case CloseCode::DISALLOWED_INTENTS:
        return true;
    default:
        return false;
    }
}

} // namespace Discord
} // namespace Acheron
