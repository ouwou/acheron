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

// One RTC connection and the thread it runs on. The voice connection is one of these, and so is
// each Go Live stream connection.
//
// The client lives on the connection's thread, and this object is its face on the thread that
// made it: the client's signals come out of it there, from start() until stop() and never after,
// so whatever a connection that has been stopped still had on its way is dropped and cannot be
// taken for the next connection's.
class VoiceConnection : public QObject
{
    Q_OBJECT
public:
    explicit VoiceConnection(const Discord::Voice::ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceConnection() override;

    // For what has to be connected on the connection's own thread, before start(): the audio
    // pipeline, the video frame handler, encoded frames to send.
    [[nodiscard]] Discord::Voice::VoiceClient *client() const { return voiceClient; }
    [[nodiscard]] QThread *networkThread() const { return connectionThread; }
    [[nodiscard]] bool isRunning() const { return connectionThread->isRunning(); }

    // Objects that have to live on the connection's thread while it runs. They come back to
    // this object's thread when the connection stops.
    void adopt(QObject *object);

    // Refuses, before any socket opens, when the account's proxy cannot carry UDP: every RTC
    // connection sends its media over UDP, and falling back to a direct route would leak it.
    bool start();

    // `beforeStop` runs on the connection's thread ahead of the client being stopped.
    void stop(const std::function<void()> &beforeStop = {});

    // Calls the client on its own thread, which is how everything is asked of it once it runs.
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
