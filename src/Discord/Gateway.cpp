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

#include <QUrl>

#include <cstdlib>

namespace Acheron {
namespace Discord {

static size_t write_cb(char *b, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

Gateway::Gateway(const QString &token, const QString &gatewayUrl, ClientIdentity &identity,
                 QObject *parent)
    : QObject(parent), token(token), gatewayUrl(gatewayUrl), identity(identity), running(false)
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

    wantToClose = false;

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

    ingest->stop();
    ingest->deleteLater();
}

void Gateway::hardStop()
{
    shouldReconnect = false;
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
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

void Gateway::sendPayload(const QJsonObject &obj)
{
    sendPayload(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Gateway::sendPayload(const QByteArray &data)
{
    std::lock_guard lock(curlMutex);

    if (!curl)
        return;

    const char *payload = data.constData();
    size_t totalBytes = data.size();
    size_t bytesSentTotal = 0;

    while (bytesSentTotal < totalBytes) {
        size_t bytesSentNow = 0;
        CURLcode res = curl_ws_send(curl, payload + bytesSentTotal, totalBytes - bytesSentTotal,
                                    &bytesSentNow, 0, CURLWS_TEXT);

        if (res == CURLE_OK) {
            bytesSentTotal += bytesSentNow;
        } else if (res == CURLE_AGAIN) {
            curl_socket_t sockfd;
            curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);

            if (sockfd != CURL_SOCKET_BAD) {
                timeval timeout{ 0, 100'000 };
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(sockfd, &writefds);

                select((int)sockfd + 1, nullptr, &writefds, nullptr, &timeout);
            } else {
                // apocalypse
            }
        } else {
            qCWarning(LogDiscord) << "Error sending payload: " << curl_easy_strerror(res);
            break;
        }
    }
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
    case OpCode::HEARTBEAT_ACK:
        heartbeatAckReceived = true;
        break;
    case OpCode::RECONNECT:
        qCInfo(LogDiscord) << "Server requested reconnect";
        shouldReconnect = true;
        break;
    case OpCode::INVALID_SESSION: {
        bool resumable = msg.data.toBool();
        qCInfo(LogDiscord) << "Invalid session, resumable:" << resumable;
        if (!resumable) {
            canResume = false;
            sessionId.clear();
        }
        shouldReconnect = true;
        break;
    }
    default:
        break;
    }
}

void Gateway::handleDispatch(const Inbound &data)
{
    QString t = data.t.value_or("");
    qCDebug(LogDiscord) << "Received dispatch event" << t;

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
    case GatewayEvent::GUILD_MEMBERS_CHUNK:
        handleGuildMembersChunk(data);
        break;
    case GatewayEvent::GUILD_MEMBER_UPDATE:
        handleGuildMemberUpdate(data);
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
    case GatewayEvent::GUILD_MEMBER_LIST_UPDATE:
        handleGuildMemberListUpdate(data);
        break;
    case GatewayEvent::VOICE_STATE_UPDATE:
        handleVoiceStateUpdate(data);
        break;
    case GatewayEvent::VOICE_SERVER_UPDATE:
        handleVoiceServerUpdate(data);
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

    if (msg.sessionId.hasValue())
        sessionId = msg.sessionId.get();
    if (msg.resumeGatewayUrl.hasValue())
        resumeGatewayUrl = msg.resumeGatewayUrl.get();
    canResume = !sessionId.isEmpty();
    reconnectAttempts = 0;

    emit gatewayReady(msg);
}

void Gateway::handleReadySupplemental(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received ready supplemental event";

    ReadySupplemental msg = data.getData<ReadySupplemental>();

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

void Gateway::handleGuildMembersChunk(const Inbound &data)
{
    GuildMembersChunk chunk = data.getData<GuildMembersChunk>();

    emit gatewayGuildMembersChunk(chunk);
}

void Gateway::handleGuildMemberUpdate(const Inbound &data)
{
    GuildMemberUpdate event = data.getData<GuildMemberUpdate>();

    emit gatewayGuildMemberUpdate(event);
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

void Gateway::handleVoiceServerUpdate(const Inbound &data)
{
    VoiceServerUpdate event = data.getData<VoiceServerUpdate>();

    emit gatewayVoiceServerUpdate(event);
}

void Gateway::requestGuildMembers(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds)
{
    RequestGuildMembers request;
    request.guildId = guildId;
    request.userIds = userIds;
    request.presences = false;

    sendPayload(request.toJson());
}

void Gateway::sendVoiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf)
{
    UpdateVoiceState msg;
    msg.guildId = guildId;
    if (channelId.isValid())
        msg.channelId = channelId;
    else
        msg.channelId = nullptr;
    msg.selfMute = selfMute;
    msg.selfDeaf = selfDeaf;

    sendPayload(msg.toJson());
}

void Gateway::handleHello(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received hello";

    Hello msg = data.getData<Hello>();

    heartbeatInterval = msg.heartbeatInterval;
    heartbeatAckReceived = true;

    if (isResuming && canResume)
        resume();
    else
        identify();

    reconnectAttempts = 0;
    isResuming = false;

    if (!heartbeatThread.joinable())
        heartbeatThread = std::thread(&Gateway::heartbeatLoop, this);

    emit gatewayHello();
}

void Gateway::identify()
{
    ClientPropertiesBuildParams params;
    params.clientAppState = "focused";
    params.includeClientHeartbeatSessionId = false;
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
    do {
        shouldReconnect = false;

        // Choose URL: use resumeGatewayUrl if resuming, else gatewayUrl.
        // resumeGatewayUrl from READY is a bare host (e.g. wss://gateway-us-east1-b.discord.gg)
        // without query parameters — append them from the original gatewayUrl.
        QString connectUrl = gatewayUrl;
        if (isResuming && canResume && !resumeGatewayUrl.isEmpty()) {
            QUrl resumeUrl(resumeGatewayUrl);
            QUrl originalUrl(gatewayUrl);
            resumeUrl.setQuery(originalUrl.query());
            connectUrl = resumeUrl.toString();
        }

        curl = curl_easy_init();
        if (!curl) {
            qCCritical(LogDiscord) << "Failed to initialize curl";
            return;
        }

        curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        printf("SSL backend: %s\n", info->ssl_version);

        QString certPath = CurlUtils::getCertificatePath();
        if (!certPath.isEmpty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, certPath.toUtf8().constData());

        curl_easy_setopt(curl, CURLOPT_URL, connectUrl.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
#ifdef IS_CURL_IMPERSONATE
        curl_easy_impersonate(curl, CurlUtils::getImpersonateTarget().toUtf8().constData(), 1);
#endif
        curl_easy_setopt(curl, CURLOPT_USERAGENT, CurlUtils::getUserAgent().toUtf8().constData());

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            qWarning() << "Failed to connect to gateway:" << curl_easy_strerror(res);

            // On connect failure during reconnect, retry with backoff
            if (isResuming && reconnectAttempts < maxReconnectAttempts) {
                std::lock_guard lock(curlMutex);
                curl_easy_cleanup(curl);
                curl = nullptr;

                reconnectAttempts++;
                int delay = 1000 + (std::rand() % 4000);
                qCInfo(LogDiscord) << "Reconnect attempt" << reconnectAttempts
                                   << "in" << delay << "ms";
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                shouldReconnect = true;
                continue;
            }

            emit disconnected(CloseCode::INTERNAL,
                              QString("Failed to connect to gateway: ") + curl_easy_strerror(res));
            return;
        }

        emit connected();

        char chunk[8192];
        size_t rlen;
        const curl_ws_frame *meta;

        bool closeSent = false;
        while (running) {
            if (shouldReconnect)
                break;

            {
                std::lock_guard lock(curlMutex);

                if (wantToClose) {
                    if (!closeSent) {
                        closeSent = true;
                        uint8_t close_payload[2] = { 0x03, 0xE8 };
                        size_t bytesSent = 0;
                        curl_ws_send(curl, close_payload, sizeof(close_payload), &bytesSent, 0,
                                     CURLWS_CLOSE);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (now - closeTime > closeTimeout) {
                        running = false;
                        qCDebug(LogDiscord) << "Gateway close timeout";
                        break;
                    }
                }

                res = curl_ws_recv(curl, chunk, sizeof(chunk), &rlen, &meta);
            }

            if (res == CURLE_AGAIN || res == CURLE_GOT_NOTHING || !meta) {
                curl_socket_t sockfd;
                {
                    std::lock_guard lock(curlMutex);
                    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
                }
                if (sockfd != CURL_SOCKET_BAD) {
                    timeval timeout{ 0, 10'000 };
                    fd_set readfds;
                    FD_ZERO(&readfds);
                    FD_SET(sockfd, &readfds);
                    select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
                }

                if (shouldReconnect)
                    break;

                continue;
            }

            if (meta->flags & CURLWS_CLOSE) {
                int closeCode = 1000;
                QString closeReason;

                if (rlen >= 2) {
                    closeCode = (uint8_t(chunk[0]) << 8) | uint8_t(chunk[1]);
                    if (rlen > 2)
                        closeReason = QString::fromUtf8(chunk + 2, rlen - 2);
                }

                qCInfo(LogDiscord) << "Connection closed with code" << closeCode
                                   << "reason:" << closeReason;
                CloseCode cc = static_cast<CloseCode>(closeCode);
                emit disconnected(cc, closeReason);
                if (!wantToClose && !isFatalCloseCode(cc) && canResume)
                    shouldReconnect = true;
                break;
            }

            if (meta->flags & (CURLWS_PING | CURLWS_PONG))
                continue;

            ingest->push(QByteArray(chunk, rlen));
        }

        // Clean up current connection
        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }

        // If shouldReconnect was set (by RECONNECT/INVALID_SESSION opcode handlers
        // or by zombie detection), prepare for reconnection
        if (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts) {
            reconnectAttempts++;
            isResuming = canResume;

            // Join the heartbeat thread if it exited (e.g. zombie detection broke the loop)
            // so handleHello can start a fresh one on reconnect
            if (heartbeatThread.joinable()) {
                heartbeatCv.notify_all();
                heartbeatThread.join();
            }

            // Reset the IngestThread's zlib stream — the new connection starts a fresh
            // zlib context, so the old stream state would corrupt decompression
            ingest->reset();

            int delay = 1000 + (std::rand() % 4000);
            qCInfo(LogDiscord) << "Reconnecting in" << delay << "ms (attempt"
                               << reconnectAttempts << ")";
            emit reconnecting(reconnectAttempts, maxReconnectAttempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

    } while (shouldReconnect && running && reconnectAttempts <= maxReconnectAttempts);

    // Ensure heartbeat thread exits when the network loop is done
    running = false;
    heartbeatCv.notify_all();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void Gateway::heartbeatLoop()
{
    qCDebug(LogDiscord) << "Heartbeat loop started, interval:" << heartbeatInterval;

    while (running) {
        if (!heartbeatAckReceived) {
            qCWarning(LogDiscord) << "No heartbeat ACK received — zombie connection detected";
            shouldReconnect = true;
            break;
        }
        heartbeatAckReceived = false;

        QoSHeartbeat heartbeat;
        heartbeat.seq = lastReceivedSequence;
        heartbeat.qos->ver = 27;
        heartbeat.qos->active = true;
        heartbeat.qos->reasons = { "foregrounded" };

        sendPayload(heartbeat.toJson());

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval),
                                             [this] { return !running || shouldReconnect.load(); });

            if (stop)
                break;
        }
    }
}

void Gateway::debugForceReconnect()
{
    qCInfo(LogDiscord) << "DEBUG: Forcing reconnect (simulating op 7 RECONNECT)";
    shouldReconnect = true;
}

void Gateway::resume()
{
    qCInfo(LogDiscord) << "Sending RESUME";
    Resume resumeMsg;
    resumeMsg.token = token;
    resumeMsg.sessionId = sessionId;
    resumeMsg.seq = lastReceivedSequence.load();
    sendPayload(resumeMsg.toJson());
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
