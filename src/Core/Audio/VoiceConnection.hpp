#pragma once

#include <QList>
#include <QObject>
#include <QThread>

#include <functional>
#include <utility>

#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {
namespace Audio {

// the client lives on the connection's thread, and its signals come out of this object only between
// start() and stop(), so nothing a stopped connection still had on its way reaches the next one
class VoiceConnection : public QObject
{
    Q_OBJECT
public:
    explicit VoiceConnection(const Discord::Voice::ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceConnection() override;

    // for wiring on the connection's own thread before start()
    [[nodiscard]] Discord::Voice::VoiceClient *client() const { return voiceClient; }
    [[nodiscard]] QThread *networkThread() const { return connectionThread; }

    void adopt(QObject *object);

    // refuses before any socket opens when the account's proxy cant carry UDP: falling back to a direct
    // route would leak the media
    bool start();

    // `beforeStop` runs on the connection's thread before the client stops
    void stop(const std::function<void()> &beforeStop = {});

    template <typename Call>
    void run(Call call)
    {
        QMetaObject::invokeMethod(voiceClient, [client = voiceClient, call = std::move(call)]() mutable { call(client); });
    }

signals:
    void stateChanged(Acheron::Discord::Voice::VoiceClient::State state);
    void connected();
    void disconnected();

    void clientReachedServer(Acheron::Core::Snowflake userId);
    void clientDisconnected(Acheron::Core::Snowflake userId);
    void speakingReceived(const Acheron::Discord::Voice::SpeakingData &data);
    void privacyCodeChanged(const QString &code);

    void remoteVideoChanged(Acheron::Core::Snowflake userId, bool active, int maxWidth, int maxHeight, int maxFramerate);
    void remoteVideoLayersChanged(Acheron::Core::Snowflake userId, const QList<Acheron::Discord::Voice::VideoLayer> &layers);
    void videoStatsUpdated(const Acheron::Discord::Voice::VideoReceiveStats &stats);

    void videoSendCodecChanged(Acheron::Discord::Voice::VideoCodec codec);
    void videoSendLayersKnown(const QList<int> &qualities);
    void videoKeyframeRequested(int quality);
    void videoReceiversChanged(int receivers, int quality);
    void videoWantsReceived(int want, int pixelCount, int quality);
    void videoSendStatsUpdated(const Acheron::Discord::Voice::VideoSendStats &stats);

private:
    template <typename Signal, typename Relay>
    void relay(Signal signal, Relay relayed);

    Discord::Voice::ConnectionParams params;
    Discord::Voice::VoiceClient *voiceClient;
    QThread *connectionThread;
    QList<QObject *> adopted;
    bool stopped = false;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
