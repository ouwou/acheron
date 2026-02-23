#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <curl/curl.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

class VoiceGateway : public QObject
{
    Q_OBJECT
public:
    VoiceGateway(const QString &endpoint, Core::Snowflake serverId, Core::Snowflake channelId,
                 Core::Snowflake userId, const QString &sessionId, const QString &token,
                 QObject *parent = nullptr);
    ~VoiceGateway();

    void start();
    void stop();
    void hardStop();

    void sendSelectProtocol(const QString &address, int port, const QString &mode);
    void sendSpeaking(int flags, int delay, quint32 ssrc);
    void sendBinaryPayload(int opcode, const QByteArray &data);
    void sendDaveReadyForTransition(int transitionId);
    void sendDaveInvalidCommitWelcome(int transitionId);

    [[nodiscard]] uint64_t currentGeneration() const { return generation.load(); }

signals:
    void connected();
    void disconnected(VoiceCloseCode code, const QString &reason);
    void reconnecting(int attempt, int maxAttempts);

    void helloReceived(int heartbeatInterval);
    void readyReceived(const VoiceReady &data);
    void sessionDescriptionReceived(const SessionDescription &data);
    void speakingReceived(const SpeakingData &data);
    void clientConnected(const ClientConnectData &data);
    void clientsConnected(const QStringList &userIds);
    void clientDisconnected(Core::Snowflake userId);
    void resumed();

    void payloadReceived(const QJsonObject &root);

    void binaryPayloadReceived(int opcode, const QByteArray &payload);
    void daveTransitionPrepare(int protocolVersion, int transitionId);
    void daveTransitionExecute(int transitionId);
    void daveEpochPrepare(int protocolVersion, int epoch);

private:
    void sendPayload(const QJsonObject &obj);
    void sendPayload(const QByteArray &data);

    void onPayloadReceived(const QJsonObject &root);
    void handleHello(const QJsonObject &data);
    void handleReady(const QJsonObject &data);
    void handleSessionDescription(const QJsonObject &data);
    void handleSpeaking(const QJsonObject &data);
    void handleHeartbeatAck(quint64 nonce);
    void handleResumed();
    void handleClientConnect(const QJsonObject &data);
    void handleClientsConnected(const QJsonObject &data);
    void handleClientDisconnect(const QJsonObject &data);
    void handleDavePrepareTransition(const QJsonObject &data);
    void handleDaveExecuteTransition(const QJsonObject &data);
    void handleDavePrepareEpoch(const QJsonObject &data);

    void identify();
    void resume();
    bool isFatalCloseCode(VoiceCloseCode code) const;

    void networkLoop();
    void heartbeatLoop();

private:
    QString endpoint;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    QString sessionId;
    QString token;

    std::atomic<bool> running{ false };

    std::mutex curlMutex;
    CURL *curl = nullptr;

    std::atomic<bool> wantToClose{ false };
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout = std::chrono::milliseconds(1000);

    std::atomic<int> heartbeatInterval{ 0 };
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;

    std::atomic<bool> heartbeatAckReceived{ true };
    std::atomic<quint64> lastSentNonce{ 0 };
    std::atomic<int> lastReceivedSeq{ -1 };

    std::atomic<bool> shouldReconnect{ false };
    std::atomic<bool> canResume{ false };
    std::atomic<bool> isResuming{ false };
    std::atomic<int> reconnectAttempts{ 0 };
    static constexpr int maxReconnectAttempts = 5;

    std::atomic<uint64_t> generation{ 0 };
};

} // namespace AV
} // namespace Discord
} // namespace Acheron
