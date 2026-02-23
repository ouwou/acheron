#include "VoiceGateway.hpp"

#include "Discord/CurlUtils.hpp"
#include "Core/Logging.hpp"

#include <QJsonDocument>
#include <QDateTime>

#include <cstdlib>

namespace Acheron {
namespace Discord {
namespace AV {

VoiceGateway::VoiceGateway(const QString &endpoint, Core::Snowflake serverId,
                           Core::Snowflake channelId, Core::Snowflake userId,
                           const QString &sessionId, const QString &token,
                           QObject *parent)
    : QObject(parent),
      endpoint(endpoint),
      serverId(serverId),
      channelId(channelId),
      userId(userId),
      sessionId(sessionId),
      token(token)
{
    connect(this, &VoiceGateway::payloadReceived, this, &VoiceGateway::onPayloadReceived);
}

VoiceGateway::~VoiceGateway()
{
    hardStop();
}

void VoiceGateway::start()
{
    if (running) {
        qCWarning(LogVoice) << "Attempt to start already running voice gateway";
        return;
    }

    wantToClose = false;
    running = true;
    networkThread = std::thread(&VoiceGateway::networkLoop, this);
}

void VoiceGateway::stop()
{
    wantToClose = true;
    closeTime = std::chrono::steady_clock::now();
}

void VoiceGateway::hardStop()
{
    shouldReconnect = false;
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::sendSelectProtocol(const QString &address, int port, const QString &mode)
{
    Codec opus;
    opus.name = "opus";
    opus.payloadType = 120;
    opus.priority = 1000;
    opus.type = "audio";

    SelectProtocolData data;
    data.address = address;
    data.port = port;
    data.mode = mode;
    data.codecs = { opus };

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::SELECT_PROTOCOL);
    obj["d"] = data.toJson();
    sendPayload(obj);
}

void VoiceGateway::sendSpeaking(int flags, int delay, quint32 ssrc)
{
    QJsonObject d;
    d["speaking"] = flags;
    d["delay"] = delay;
    d["ssrc"] = static_cast<qint64>(ssrc);

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::SPEAKING);
    obj["d"] = d;
    sendPayload(obj);
}

void VoiceGateway::sendBinaryPayload(int opcode, const QByteArray &data)
{
    QByteArray frame;
    frame.reserve(1 + data.size());
    frame.append(static_cast<char>(opcode));
    frame.append(data);

    qCDebug(LogVoice) << "Voice binary >>> opcode =" << opcode << "size =" << data.size();

    std::lock_guard lock(curlMutex);
    if (!curl)
        return;

    const char *payload = frame.constData();
    size_t totalBytes = frame.size();
    size_t bytesSentTotal = 0;

    while (bytesSentTotal < totalBytes) {
        size_t bytesSentNow = 0;
        CURLcode res = curl_ws_send(curl, payload + bytesSentTotal, totalBytes - bytesSentTotal,
                                    &bytesSentNow, 0, CURLWS_BINARY);

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
            }
        } else {
            qCWarning(LogVoice) << "Error sending binary voice payload:" << curl_easy_strerror(res);
            break;
        }
    }
}

void VoiceGateway::sendDaveReadyForTransition(int transitionId)
{
    QJsonObject d;
    d["transition_id"] = transitionId;

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::DAVE_PROTOCOL_READY_FOR_TRANSITION);
    obj["d"] = d;
    sendPayload(obj);
}

void VoiceGateway::sendDaveInvalidCommitWelcome(int transitionId)
{
    QJsonObject d;
    d["transition_id"] = transitionId;

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::DAVE_MLS_INVALID_COMMIT_WELCOME);
    obj["d"] = d;
    sendPayload(obj);
}

void VoiceGateway::sendPayload(const QJsonObject &obj)
{
    QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    qCDebug(LogVoice) << "Voice >>>" << json;
    sendPayload(json);
}

void VoiceGateway::sendPayload(const QByteArray &data)
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
            }
        } else {
            qCWarning(LogVoice) << "Error sending voice payload:" << curl_easy_strerror(res);
            break;
        }
    }
}

void VoiceGateway::onPayloadReceived(const QJsonObject &root)
{
    qCDebug(LogVoice) << "Voice <<<" << QJsonDocument(root).toJson(QJsonDocument::Compact);

    if (root.contains("seq") && !root["seq"].isNull())
        lastReceivedSeq = root["seq"].toInt();

    int op = root["op"].toInt(-1);
    QJsonValue d = root["d"];

    switch (static_cast<VoiceOpCode>(op)) {
    case VoiceOpCode::HELLO:
        handleHello(d.toObject());
        break;
    case VoiceOpCode::READY:
        handleReady(d.toObject());
        break;
    case VoiceOpCode::SESSION_DESCRIPTION:
        handleSessionDescription(d.toObject());
        break;
    case VoiceOpCode::SPEAKING:
        handleSpeaking(d.toObject());
        break;
    case VoiceOpCode::HEARTBEAT_ACK:
        handleHeartbeatAck(static_cast<quint64>(d.toDouble()));
        break;
    case VoiceOpCode::RESUMED:
        handleResumed();
        break;
    case VoiceOpCode::CLIENT_CONNECT:
        handleClientsConnected(d.toObject());
        break;
    case VoiceOpCode::SESSION_UPDATE:
        handleClientConnect(d.toObject());
        break;
    case VoiceOpCode::CLIENT_DISCONNECT:
        handleClientDisconnect(d.toObject());
        break;
    case VoiceOpCode::DAVE_PROTOCOL_PREPARE_TRANSITION:
        handleDavePrepareTransition(d.toObject());
        break;
    case VoiceOpCode::DAVE_PROTOCOL_EXECUTE_TRANSITION:
        handleDaveExecuteTransition(d.toObject());
        break;
    case VoiceOpCode::DAVE_PROTOCOL_PREPARE_EPOCH:
        handleDavePrepareEpoch(d.toObject());
        break;
    default:
        qCDebug(LogVoice) << "Unhandled voice opcode:" << op;
        break;
    }
}

void VoiceGateway::handleHello(const QJsonObject &data)
{
    VoiceHello hello = VoiceHello::fromJson(data);

    qCInfo(LogVoice) << "Voice Hello, heartbeat interval:" << hello.heartbeatInterval << "ms";

    heartbeatInterval = hello.heartbeatInterval;
    heartbeatAckReceived = true;

    if (isResuming && canResume)
        resume();
    else
        identify();

    reconnectAttempts = 0;
    isResuming = false;

    if (!heartbeatThread.joinable())
        heartbeatThread = std::thread(&VoiceGateway::heartbeatLoop, this);

    emit helloReceived(hello.heartbeatInterval);
}

void VoiceGateway::handleReady(const QJsonObject &data)
{
    VoiceReady ready = VoiceReady::fromJson(data);
    canResume = true;
    emit readyReceived(ready);
}

void VoiceGateway::handleSessionDescription(const QJsonObject &data)
{
    SessionDescription desc = SessionDescription::fromJson(data);
    emit sessionDescriptionReceived(desc);
}

void VoiceGateway::handleSpeaking(const QJsonObject &data)
{
    SpeakingData speaking = SpeakingData::fromJson(data);

    qCDebug(LogVoice) << "Speaking: user =" << speaking.userId.get()
                      << "ssrc =" << speaking.ssrc
                      << "flags =" << speaking.speaking;

    emit speakingReceived(speaking);
}

void VoiceGateway::handleHeartbeatAck(quint64 nonce)
{
    heartbeatAckReceived = true;

    qCDebug(LogVoice) << "Voice heartbeat ACK, nonce:" << nonce;
}

void VoiceGateway::handleResumed()
{
    qCInfo(LogVoice) << "Voice session resumed";
    canResume = true;
    emit resumed();
}

void VoiceGateway::handleClientConnect(const QJsonObject &data)
{
    ClientConnectData client = ClientConnectData::fromJson(data);

    qCInfo(LogVoice) << "Client connected: user =" << client.userId.get()
                     << "audio_ssrc =" << client.audioSsrc
                     << "video_ssrc =" << client.videoSsrc;

    emit clientConnected(client);
}

void VoiceGateway::handleClientDisconnect(const QJsonObject &data)
{
    Core::Snowflake userId = data["user_id"].toString().toULongLong();

    qCInfo(LogVoice) << "Client disconnected: user =" << userId;

    emit clientDisconnected(userId);
}

void VoiceGateway::handleClientsConnected(const QJsonObject &data)
{
    QStringList userIds;
    for (const auto &val : data["user_ids"].toArray())
        userIds.append(val.toString());

    qCInfo(LogVoice) << "Clients connected:" << userIds;

    emit clientsConnected(userIds);
}

void VoiceGateway::handleDavePrepareTransition(const QJsonObject &data)
{
    emit daveTransitionPrepare(data["protocol_version"].toInt(), data["transition_id"].toInt());
}

void VoiceGateway::handleDaveExecuteTransition(const QJsonObject &data)
{
    emit daveTransitionExecute(data["transition_id"].toInt());
}

void VoiceGateway::handleDavePrepareEpoch(const QJsonObject &data)
{
    emit daveEpochPrepare(data["protocol_version"].toInt(), data["epoch"].toInt());
}

void VoiceGateway::identify()
{
    qCInfo(LogVoice) << "Sending voice Identify for server" << serverId;

    VoiceIdentifyData id;
    id.channelId = channelId;
    id.serverId = serverId;
    id.userId = userId;
    id.sessionId = sessionId;
    id.token = token;

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::IDENTIFY);
    obj["d"] = id.toJson();
    sendPayload(obj);
}

void VoiceGateway::resume()
{
    qCInfo(LogVoice) << "Sending voice Resume";

    VoiceResumeData data;
    data.serverId = serverId;
    data.sessionId = sessionId;
    data.token = token;

    QJsonObject obj;
    obj["op"] = static_cast<int>(VoiceOpCode::RESUME);
    obj["d"] = data.toJson();
    sendPayload(obj);
}

bool VoiceGateway::isFatalCloseCode(VoiceCloseCode code) const
{
    switch (code) {
    case VoiceCloseCode::AUTHENTICATION_FAILED:
    case VoiceCloseCode::DISCONNECTED:
    case VoiceCloseCode::RATE_LIMITED:
    case VoiceCloseCode::DISCONNECTED_ALL:
        return true;
    default:
        return false;
    }
}

void VoiceGateway::networkLoop()
{
    do {
        shouldReconnect = false;

        QString connectUrl = QStringLiteral("wss://%1/?v=%2")
                                     .arg(endpoint)
                                     .arg(VOICE_GATEWAY_VERSION);

        curl = curl_easy_init();
        if (!curl) {
            qCCritical(LogVoice) << "Failed to initialize curl for voice gateway";
            return;
        }

        QString certPath = CurlUtils::getCertificatePath();
        if (!certPath.isEmpty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, certPath.toUtf8().constData());

        curl_easy_setopt(curl, CURLOPT_URL, connectUrl.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
#if 0
        curl_easy_setopt(curl, CURLOPT_PROXY, "http://127.0.0.1:8888");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
#ifdef IS_CURL_IMPERSONATE
        curl_easy_impersonate(curl, CurlUtils::getImpersonateTarget().toUtf8().constData(), 1);
#endif
        curl_easy_setopt(curl, CURLOPT_USERAGENT, CurlUtils::getUserAgent().toUtf8().constData());

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            qCWarning(LogVoice) << "Failed to connect to voice gateway:"
                                << curl_easy_strerror(res);

            if (isResuming && reconnectAttempts < maxReconnectAttempts) {
                std::lock_guard lock(curlMutex);
                curl_easy_cleanup(curl);
                curl = nullptr;

                reconnectAttempts++;
                int delay = 1000 + (std::rand() % 4000);
                qCInfo(LogVoice) << "Voice reconnect attempt" << reconnectAttempts
                                 << "in" << delay << "ms";
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                shouldReconnect = true;
                continue;
            }

            emit disconnected(VoiceCloseCode::INTERNAL,
                              QString("Failed to connect to voice gateway: ") + curl_easy_strerror(res));
            return;
        }

        qCInfo(LogVoice) << "Voice WebSocket connected to" << endpoint;
        generation++;
        emit connected();

        char chunk[8192];
        size_t rlen;
        const curl_ws_frame *meta;
        QByteArray frameBuffer;
        QByteArray binaryFrameBuffer;

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
                        curl_ws_send(curl, close_payload, sizeof(close_payload),
                                     &bytesSent, 0, CURLWS_CLOSE);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (now - closeTime > closeTimeout) {
                        running = false;
                        qCDebug(LogVoice) << "Voice gateway close timeout";
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

                qCInfo(LogVoice) << "Voice connection closed with code" << closeCode
                                 << "reason:" << closeReason;

                VoiceCloseCode cc = static_cast<VoiceCloseCode>(closeCode);
                emit disconnected(cc, closeReason);

                if (!wantToClose && !isFatalCloseCode(cc) && canResume)
                    shouldReconnect = true;
                break;
            }

            if (meta->flags & (CURLWS_PING | CURLWS_PONG))
                continue;

            if (meta->flags & CURLWS_BINARY) {
                binaryFrameBuffer.append(chunk, rlen);

                if (meta->bytesleft == 0) {
                    if (binaryFrameBuffer.size() >= 3) {
                        const auto *raw = reinterpret_cast<const uint8_t *>(binaryFrameBuffer.constData());
                        // uint16_t seq = (raw[0] << 8) | raw[1];
                        int opcode = raw[2];
                        QByteArray payload = binaryFrameBuffer.mid(3);

                        qCDebug(LogVoice) << "Voice binary frame: opcode =" << opcode
                                          << "payload size =" << payload.size();
                        emit binaryPayloadReceived(opcode, payload);
                    }
                    binaryFrameBuffer.clear();
                }
                continue;
            }

            frameBuffer.append(chunk, rlen);

            if (meta->bytesleft == 0) {
                QJsonParseError error;
                QJsonDocument doc = QJsonDocument::fromJson(frameBuffer, &error);
                frameBuffer.clear();

                if (error.error != QJsonParseError::NoError) {
                    qCWarning(LogVoice) << "Failed to parse voice payload:"
                                        << error.errorString();
                } else if (doc.isObject()) {
                    emit payloadReceived(doc.object());
                }
            }
        }

        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }

        if (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts) {
            reconnectAttempts++;
            isResuming = canResume.load();

            if (heartbeatThread.joinable()) {
                heartbeatCv.notify_all();
                heartbeatThread.join();
            }

            int delay = 1000 + (std::rand() % 4000);
            qCInfo(LogVoice) << "Voice reconnecting in" << delay << "ms (attempt"
                             << reconnectAttempts << ")";
            emit reconnecting(reconnectAttempts, maxReconnectAttempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

    } while (shouldReconnect && running && reconnectAttempts <= maxReconnectAttempts);

    running = false;
    heartbeatCv.notify_all();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::heartbeatLoop()
{
    qCDebug(LogVoice) << "Voice heartbeat loop started, interval:" << heartbeatInterval;

    // wait before first heartbeat
    {
        std::unique_lock lock(heartbeatMutex);
        bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval),
                                         [this] { return !running || shouldReconnect.load(); });
        if (stop)
            return;
    }

    while (running) {
        if (!heartbeatAckReceived) {
            qCWarning(LogVoice) << "No voice heartbeat ACK — zombie connection";
            shouldReconnect = true;
            break;
        }
        heartbeatAckReceived = false;

        quint64 nonce = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
        lastSentNonce = nonce;

        QJsonObject d;
        d["t"] = static_cast<qint64>(nonce);
        d["seq_ack"] = lastReceivedSeq.load();

        QJsonObject obj;
        obj["op"] = static_cast<int>(VoiceOpCode::HEARTBEAT);
        obj["d"] = d;
        sendPayload(obj);

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval),
                                             [this] { return !running || shouldReconnect.load(); });
            if (stop)
                break;
        }
    }
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
