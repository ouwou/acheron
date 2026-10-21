#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class VoiceGateway : public QObject
{
    Q_OBJECT
public:
    explicit VoiceGateway(const ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceGateway();

    void start();
    void stop();
    void hardStop();

    void sendSelectProtocol(const SelectProtocolData &data);
    void sendVideo(const LocalVideoState &state);
    void sendSessionUpdate(const QList<Codec> &codecs);
    void sendMediaSinkWants(const MediaSinkWants &wants);
    void sendVoiceBackendVersion();
    void sendSpeaking(int flags, int delay, quint32 ssrc);
    void sendBinaryPayload(int opcode, const QByteArray &data);
    void sendDaveReadyForTransition(int transitionId);
    void sendDaveInvalidCommitWelcome(int transitionId);

    [[nodiscard]] uint64_t currentGeneration() const { return generation.load(); }
    // any thread
    [[nodiscard]] bool isStopping() const { return !running; }

signals:
    void connected();
    void disconnected(Acheron::Discord::Voice::VoiceCloseCode code, const QString &reason);
    void reconnecting(int attempt, int maxAttempts);

    void helloReceived(int heartbeatInterval);
    void readyReceived(const VoiceReady &data);
    void sessionDescriptionReceived(const SessionDescription &data);
    void speakingReceived(const SpeakingData &data);
    void clientConnected(const ClientConnectData &data);
    void clientsConnected(const QStringList &userIds);
    void clientDisconnected(Core::Snowflake userId);
    void sessionUpdated(const SessionUpdate &update);
    void mediaSinkWantsReceived(const MediaSinkWants &wants);
    void resumed();

    void payloadReceived(const QJsonObject &root);

    void binaryPayloadReceived(int opcode, const QByteArray &payload);
    void daveTransitionPrepare(int protocolVersion, int transitionId);
    void daveTransitionExecute(int transitionId);
    void daveEpochPrepare(int protocolVersion, int epoch);

private:
    void sendPayload(VoiceOpCode op, const Core::OrderedJson &d);
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
    void waitUnlessStopped(std::chrono::milliseconds delay);

private:
    ConnectionParams params;

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

} // namespace Voice
} // namespace Discord
} // namespace Acheron
