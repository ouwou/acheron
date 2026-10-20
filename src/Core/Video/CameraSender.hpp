#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <array>
#include <functional>
#include <memory>

#include "Core/Snowflake.hpp"
#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Discord {
class Client;
}

namespace Core {

class StreamBroadcaster;

namespace Audio {
class VoiceConnection;
class VoiceManager;
} // namespace Audio

namespace Video {
class CameraPipeline;
}

// The account's own camera in the voice channel or call it is connected to. Says on the
// gateway that the camera is on, runs the camera's pipeline, and sends what it encodes on the
// voice connection, at the size, frame rate and bitrate the official client's rules give.
//
// The camera goes off by itself when the account leaves the channel or is moved to another,
// when another of its sessions takes the call over, and when the camera fails.
class CameraSender : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Off,
        // The gateway has been told; the camera, its encoder or the voice connection is not
        // up yet.
        Starting,
        On,
        // Off, having gone off by itself. failure() says why.
        Failed,
    };
    Q_ENUM(State)

    struct Stats
    {
        Video::BroadcastStats encoder;
        Discord::Voice::VideoSendStats network;
        // What the encoder is asked for, in bits per second.
        int targetBitrate = 0;
        // As the server counts them; negative until it has said.
        int receivers = -1;
        // How much of the picture its viewers want, 0 to 100, as the encoder now serves it.
        int servedWant = 100;
        bool mutedFramerate = false;
        bool encodingSuspended = false;

        // The second layer, where the connection has one: the same again for it.
        bool hasSecondLayer = false;
        Video::BroadcastStats secondEncoder;
        int secondTargetBitrate = 0;
        int secondReceivers = -1;
        int secondServedWant = 100;
        bool secondEncodingSuspended = true;
    };

    // Whether the account may turn its camera on where it is in voice now.
    using Permission = std::function<bool()>;

    CameraSender(Discord::Client *client, Audio::VoiceManager *voice, StreamBroadcaster *screenShare, Permission allowedHere, QObject *parent = nullptr);
    ~CameraSender() override;

    // A new gateway session has forgotten the camera, so nothing is sent for it.
    void reset();

    [[nodiscard]] bool canTurnOn() const;
    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isOn() const { return currentState == State::Starting || currentState == State::On; }
    // What a voice state update for `channelId` has to say of the camera.
    [[nodiscard]] bool isOnIn(Snowflake channelId) const { return isOn() && channelId == cameraChannelId; }
    [[nodiscard]] Stats stats() const;
    // The camera's latest picture, as the camera sees it. Null until there is one.
    [[nodiscard]] QImage preview() const { return latestPreview; }
    // Why the camera went off by itself, in one sentence. Empty unless the state is Failed.
    [[nodiscard]] QString failure() const { return failureText; }
    void clearFailure();

    // An empty id takes the first camera there is. While the camera is on, this changes to
    // another camera without turning it off.
    void turnOn(const QString &deviceId);
    void turnOff();

    // How long the server is given to name a codec the camera can be sent with.
    static constexpr int CODEC_TIMEOUT_MS = 8000;

signals:
    // Also sent when the first picture of a camera arrives.
    void stateChanged();
    void previewChanged(const QImage &preview);
    // The camera went off without being asked to. failure() has the same sentence.
    void failed(const QString &reason);

private slots:
    void onRtcConnectionCreated(Acheron::Core::Audio::VoiceConnection *connection);
    void onRtcConnectionClosing();
    void onVoiceDisconnected();
    void onVoiceMembershipChanged();
    void onOwnVoiceStateChanged();
    void onSpeakingChanged(bool nowSpeaking);
    void onScreenShareChanged();
    void onCodecTimedOut();
    void applyPlan();
    void onRtcStateChanged(Acheron::Discord::Voice::VoiceClient::State state);
    void onSendCodecChanged(Acheron::Discord::Voice::VideoCodec codec);
    void onSendStats(const Acheron::Discord::Voice::VideoSendStats &stats);
    void onSendLayersKnown(const QList<int> &qualities);
    void onKeyframeRequested(int quality);
    void onReceiversChanged(int count, int quality);

private:
    void onWantsReceived(int want, int quality);
    void onEncodersKnown(const QList<Discord::Voice::VideoCodec> &codecs);
    void onPipelineStarted(const QSize &encodedSize);
    void onPreviewReady(const QImage &preview);
    void onPipelineFailed(const QString &reason);
    void onPipelineStats(const Video::BroadcastStats &stats, int layer);

    void setState(State state);
    void end(bool tellGateway, State endState);
    void fail(const QString &reason, bool tellGateway = true);
    void sendVoiceState();

    void sendOnceReady();
    void startPipeline();
    void stopPipeline();
    void sendFramesToRtc();
    void announceVideo();
    void applySuspension();

    Discord::Client *client;
    Audio::VoiceManager *voice;
    StreamBroadcaster *screenShare;
    Permission allowedHere;

    State currentState = State::Off;
    QString failureText;
    Snowflake cameraChannelId;
    QString deviceId;
    bool encodersKnown = false;
    quint64 encoderProbeAttempt = 0;

    // The voice connection. Null between connections.
    Audio::VoiceConnection *rtc = nullptr;
    bool rtcConnected = false;
    Discord::Voice::VideoCodec sendCodec = Discord::Voice::VideoCodec::Unknown;
    bool videoAnnounced = false;

    // One encoding of the camera, by its layer in the pipeline: the full picture, and beside
    // it the second layer, which is sent only where the connection has one.
    struct Layer
    {
        int quality = Video::CAMERA_FULL_QUALITY;
        Video::CameraSendPlan plan;
        Video::CameraEncoding asked;
        // As the server counts them; negative until it has said.
        int receivers = -1;
        bool suspended = false;
        Video::BroadcastStats encoderStats;
    };
    static constexpr int FULL = 0;
    static constexpr int SECOND = 1;
    [[nodiscard]] int layerOf(int quality) const { return quality == layers[SECOND].quality ? SECOND : FULL; }

    std::array<Layer, 2> layers;
    bool hasSecondLayer = false;

    std::unique_ptr<Video::CameraPipeline> pipeline;
    Discord::Voice::VideoCodec pipelineCodec = Discord::Voice::VideoCodec::Unknown;
    QMetaObject::Connection framesToRtc;
    bool pipelineStarted = false;

    bool speaking = false;
    QElapsedTimer clock;
    QTimer planTimer;
    QTimer codecTimeout;

    QImage latestPreview;
    Discord::Voice::VideoSendStats networkStats;
};

} // namespace Core
} // namespace Acheron
