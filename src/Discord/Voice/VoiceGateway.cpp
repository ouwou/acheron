#include "VoiceGateway.hpp"

#include "Discord/CurlUtils.hpp"
#include "Core/Logging.hpp"

#include <QJsonDocument>
#include <QDateTime>

#include <cstdlib>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

// lets a stopped connection go at once while its proxy or server keeps it waiting
int abortOnceStopped(void *gateway, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const VoiceGateway *>(gateway)->isStopping() ? 1 : 0;
}

} // namespace

VoiceGateway::VoiceGateway(const ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params)
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
    {
        std::lock_guard lock(heartbeatMutex);
        running = false;
    }

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::sendSelectProtocol(const SelectProtocolData &data)
{
    sendPayload(VoiceOpCode::SELECT_PROTOCOL, data.toJson());
}

void VoiceGateway::sendVideo(const LocalVideoState &state)
{
    sendPayload(VoiceOpCode::VIDEO, state.toJson());
}

void VoiceGateway::sendSessionUpdate(const QList<Codec> &codecs)
{
    Core::OrderedJson::Array array;
    for (const Codec &codec : codecs)
        array.append(codec.toJson());

    Core::OrderedJson d;
    d.insert("codecs", array);
    sendPayload(VoiceOpCode::SESSION_UPDATE, d);
}

void VoiceGateway::sendMediaSinkWants(const MediaSinkWants &wants)
{
    sendPayload(VoiceOpCode::MEDIA_SINK_WANTS, wants.toJson());
}

void VoiceGateway::sendVoiceBackendVersion()
{
    sendPayload(VoiceOpCode::VOICE_BACKEND_VERSION, Core::OrderedJson());
}

void VoiceGateway::sendSpeaking(int flags, int delay, quint32 ssrc)
{
    Core::OrderedJson d;
    d.insert("speaking", flags);
    d.insert("delay", delay);
    d.insert("ssrc", static_cast<qint64>(ssrc));
    sendPayload(VoiceOpCode::SPEAKING, d);
}

void VoiceGateway::sendBinaryPayload(int opcode, const QByteArray &data)
{
    QByteArray frame;
    frame.reserve(1 + data.size());
    frame.append(static_cast<char>(opcode));
    frame.append(data);

    qCDebug(LogVoice) << "Voice binary >>> opcode =" << opcode << "size =" << data.size();

    CurlUtils::wsSend(curl, curlMutex, frame.constData(), frame.size(), CURLWS_BINARY, "voice binary");
}

void VoiceGateway::sendDaveReadyForTransition(int transitionId)
{
    Core::OrderedJson d;
    d.insert("transition_id", transitionId);
    sendPayload(VoiceOpCode::DAVE_PROTOCOL_READY_FOR_TRANSITION, d);
}

void VoiceGateway::sendDaveInvalidCommitWelcome(int transitionId)
{
    Core::OrderedJson d;
    d.insert("transition_id", transitionId);
    sendPayload(VoiceOpCode::DAVE_MLS_INVALID_COMMIT_WELCOME, d);
}

void VoiceGateway::sendPayload(VoiceOpCode op, const Core::OrderedJson &d)
{
    Core::OrderedJson obj;
    obj.insert("op", static_cast<int>(op));
    obj.insert("d", d);

    QByteArray json = obj.toBytes();
    qCDebug(LogVoice) << "Voice >>>" << json;
    sendPayload(json);
}

void VoiceGateway::sendPayload(const QByteArray &data)
{
    CurlUtils::wsSend(curl, curlMutex, data.constData(), data.size(), CURLWS_TEXT, "voice");
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
    case VoiceOpCode::VIDEO:
        handleClientConnect(d.toObject());
        break;
    case VoiceOpCode::SESSION_UPDATE:
        emit sessionUpdated(SessionUpdate::fromJson(d.toObject()));
        break;
    case VoiceOpCode::MEDIA_SINK_WANTS:
        emit mediaSinkWantsReceived(MediaSinkWants::fromJson(d.toObject()));
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
    qCInfo(LogVoice) << "Sending voice Identify for server" << params.serverId;

    VoiceIdentifyData id;
    id.channelId = params.channelId;
    id.serverId = params.serverId;
    id.userId = params.userId;
    id.sessionId = params.sessionId;
    id.token = params.token;
    if (params.video) {
        id.video = true;
        id.streams = params.streams;
    }
    sendPayload(VoiceOpCode::IDENTIFY, id.toJson());
}

void VoiceGateway::resume()
{
    qCInfo(LogVoice) << "Sending voice Resume";

    VoiceResumeData data;
    data.token = params.token;
    data.sessionId = params.sessionId;
    data.serverId = params.serverId;
    data.channelId = params.channelId;
    data.seqAck = lastReceivedSeq.load();
    sendPayload(VoiceOpCode::RESUME, data.toJson());
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
    VoiceCloseCode lastCloseCode = VoiceCloseCode::INTERNAL;
    QString lastCloseReason;
    bool terminalEmitted = false;

    do {
        shouldReconnect = false;

        QString connectUrl = QStringLiteral("%1://%2/?v=%3")
                                     .arg(params.gatewayScheme, params.endpoint)
                                     .arg(VOICE_GATEWAY_VERSION);

        curl = curl_easy_init();
        if (!curl) {
            qCCritical(LogVoice) << "Failed to initialize curl for voice gateway";
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, connectUrl.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortOnceStopped);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        CurlUtils::applyCommonOptions(curl);
        CurlUtils::applyProxy(curl, params.proxy);

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
                waitUnlessStopped(std::chrono::milliseconds(delay));
                shouldReconnect = true;
                continue;
            }

            emit disconnected(VoiceCloseCode::INTERNAL,
                              QString("Failed to connect to voice gateway: ") + curl_easy_strerror(res));
            return;
        }

        qCInfo(LogVoice) << "Voice WebSocket connected to" << params.endpoint;
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
                CurlUtils::wsRecvWait(curl, curlMutex);

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

                if (!wantToClose && !isFatalCloseCode(cc) && canResume) {
                    lastCloseCode = cc;
                    lastCloseReason = closeReason;
                    shouldReconnect = true;
                } else {
                    terminalEmitted = true;
                    emit disconnected(cc, closeReason);
                }
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
            waitUnlessStopped(std::chrono::milliseconds(delay));
        }

    } while (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts);

    if (running && !wantToClose && !terminalEmitted)
        emit disconnected(lastCloseCode, lastCloseReason);

    running = false;
    heartbeatCv.notify_all();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::waitUnlessStopped(std::chrono::milliseconds delay)
{
    std::unique_lock lock(heartbeatMutex);
    heartbeatCv.wait_for(lock, delay, [this] { return !running; });
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

        Core::OrderedJson d;
        d.insert("t", static_cast<qint64>(nonce));
        d.insert("seq_ack", lastReceivedSeq.load());
        sendPayload(VoiceOpCode::HEARTBEAT, d);

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval),
                                             [this] { return !running || shouldReconnect.load(); });
            if (stop)
                break;
        }
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
