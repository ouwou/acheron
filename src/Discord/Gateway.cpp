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
    // running = false;

    wantToClose = true;
    closeTime = std::chrono::steady_clock::now();

    ingest->stop();
    ingest->deleteLater();
}

void Gateway::hardStop()
{
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void Gateway::subscribeToGuild(Core::Snowflake guildId)
{
    GuildSubscriptionsBulk data;
    GuildSubscriptionsBulk::SubscriptionData guild;
    guild.typing = true;
    guild.activities = true;
    guild.threads = true;
    data.subscriptions.get().insert(guildId, guild);

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
    case GatewayEvent::GUILD_ROLE_CREATE:
        handleGuildRoleCreate(data);
        break;
    case GatewayEvent::GUILD_ROLE_UPDATE:
        handleGuildRoleUpdate(data);
        break;
    case GatewayEvent::GUILD_ROLE_DELETE:
        handleGuildRoleDelete(data);
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

void Gateway::requestGuildMembers(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds)
{
    RequestGuildMembers request;
    request.guildId = guildId;
    request.userIds = userIds;
    request.presences = false;

    sendPayload(request.toJson());
}

void Gateway::handleHello(const Inbound &data)
{
    qCDebug(LogDiscord) << "Received hello";

    Hello msg = data.getData<Hello>();

    heartbeatInterval = msg.heartbeatInterval;

    if (heartbeatThread.joinable()) {
        qCWarning(LogDiscord) << "Heartbeat thread already running";
        return;
    }

    identify();

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

    curl_easy_setopt(curl, CURLOPT_URL, gatewayUrl.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
#ifdef IS_CURL_IMPERSONATE
    curl_easy_impersonate(curl, CurlUtils::getImpersonateTarget().toUtf8().constData(), 1);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, CurlUtils::getUserAgent().toUtf8().constData());
    // curl_easy_setopt(curl, CURLOPT_PROXY, "http://127.0.0.1:8888");
    // dont verify
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    // curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curlDebug);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        qWarning() << "Failed to connect to gateway:" << curl_easy_strerror(res);
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

            continue;
        }

        if (meta->flags & CURLWS_CLOSE) {
            int closeCode = 1000;
            QString closeReason;

            if (rlen >= 2) {
                closeCode = (uint8_t(chunk[0]) << 8) | uint8_t(chunk[1]);
                if (rlen > 2) {
                    closeReason = QString::fromUtf8(chunk + 2, rlen - 2);
                }
            }

            qCInfo(LogDiscord) << "Connection closed with code" << closeCode
                               << "reason:" << closeReason;
            emit disconnected(static_cast<CloseCode>(closeCode), closeReason);
            break;
        }

        if (meta->flags & (CURLWS_PING | CURLWS_PONG))
            continue;

        ingest->push(QByteArray(chunk, rlen));
    }

    {
        std::lock_guard lock(curlMutex);

        curl_easy_cleanup(curl);
        curl = nullptr;
    }
}

void Gateway::heartbeatLoop()
{
    qCDebug(LogDiscord) << "Heartbeat loop started, interval:" << heartbeatInterval;

    while (running) {
        QoSHeartbeat heartbeat;
        heartbeat.seq = lastReceivedSequence;
        heartbeat.qos->ver = 27;
        heartbeat.qos->active = true;
        heartbeat.qos->reasons = { "foregrounded" };

        sendPayload(heartbeat.toJson());

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval),
                                             [this] { return !running; });

            if (stop)
                break;
        }
    }
}

} // namespace Discord
} // namespace Acheron
