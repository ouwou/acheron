#include "Gateway.hpp"

#include "Enums.hpp"
#include "Objects.hpp"
#include "Outbound.hpp"
#include "Inbound.hpp"
#include "Events.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {

static size_t write_cb(char *b, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

Gateway::Gateway(const QString &token, const QString &gatewayUrl, QObject *parent)
    : QObject(parent), token(token), gatewayUrl(gatewayUrl), running(false)
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
        }
    }
}

void Gateway::onPayloadReceived(const QJsonObject &root)
{
    qCDebug(LogDiscord) << "Received payload:"
                        << QJsonDocument(root).toJson(QJsonDocument::Compact);

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

void Gateway::handleReadySupplemental(const Inbound &data) { }

void Gateway::handleMessageCreate(const Inbound &data)
{
    Message msg = data.getData<Message>();

    emit gatewayMessageCreate(msg);
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
    launchSignature = generateLaunchSignature();
    launchId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    ClientProperties properties;
    properties.os = "Windows";
    properties.browser = "Firefox";
    properties.device = "";
    properties.systemLocale = "en-US";
    properties.hasClientMods = false;
    properties.browserUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) "
                                  "Gecko/20100101 Firefox/148.0";
    properties.browserVersion = "148.0";
    properties.osVersion = "10";
    properties.referrer = "https://discord.com/";
    properties.referringDomain = "discord.com";
    properties.referrerCurrent = "";
    properties.referringDomainCurrent = "";
    properties.releaseChannel = "stable";
    properties.clientBuildNumber = 482285;
    properties.clientEventSource = nullptr;
    properties.clientLaunchId = launchId;
    properties.launchSignature = launchSignature;
    properties.clientAppState = "unfocused";

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

QString Gateway::generateLaunchSignature()
{
    QUuid uuid = QUuid::createUuid();
    QByteArray bytes = uuid.toRfc4122();

    static constexpr quint8 mask[16] = {
        0xff, 0x7f, 0xef, 0xef, 0xf7, 0xef, 0xf7, 0xff,
        0xdf, 0x7e, 0xff, 0xbf, 0xfe, 0xff, 0xf7, 0xff,
    };

    for (int i = 0; i < 16; i++)
        bytes[i] = static_cast<char>(static_cast<quint8>(bytes[i]) & static_cast<quint8>(mask[i]));

    QUuid signature = QUuid::fromRfc4122(bytes);
    return signature.toString(QUuid::WithoutBraces);
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

    curl_easy_setopt(curl, CURLOPT_URL, gatewayUrl.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    // curl_easy_setopt(curl, CURLOPT_PROXY, "http://127.0.0.1:8888");
    // dont verify
    // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    // curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curlDebug);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        qWarning() << "Failed to connect to gateway:" << curl_easy_strerror(res);
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

        if (res == CURLE_AGAIN) {
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
