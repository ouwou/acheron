#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include "Core/GatewayNotice.hpp"
#include "Core/Media/CodecSupport.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Video/EncodePipeline.hpp"
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

// goes off by itself on leaving or moving channel, when another session takes the call over, and when
// the camera fails
class CameraSender : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Off,
        Starting,
        On,
        Failed,
    };
    Q_ENUM(State)

    struct LayerStats
    {
        Video::PipelineStats encoder;
        int targetBitrateBps = 0;
        std::optional<int> receivers;
        int servedWant = Discord::Voice::FULL_QUALITY;
        bool encodingSuspended = true;
    };

    struct Stats
    {
        Discord::Voice::VideoSendStats network;
        bool mutedFramerate = false;
        std::array<LayerStats, Video::CAMERA_LAYERS> layers;
        bool hasSecondLayer = false;
    };

    using Permission = std::function<bool()>;

    CameraSender(Discord::Client *client, Audio::VoiceManager *voice, StreamBroadcaster *screenShare, Permission allowedHere, QObject *parent = nullptr);
    ~CameraSender() override;

    void reset();

    [[nodiscard]] bool canTurnOn() const;
    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isOn() const { return currentState == State::Starting || currentState == State::On; }
    [[nodiscard]] bool isOnIn(Snowflake channelId) const { return isOn() && channelId == cameraChannelId; }
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] QImage preview() const { return latestPreview; }
    [[nodiscard]] QString failure() const { return failureText; }
    void clearFailure();

    void turnOn(const QString &deviceId);
    void turnOff();

signals:
    void stateChanged();
    void previewChanged(const QImage &preview);
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
    void onPipelineStats(const Video::PipelineStats &stats, int layer);

    void setState(State state);
    void end(GatewayNotice notice, State endState);
    void fail(const QString &reason, GatewayNotice notice = GatewayNotice::Send);
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
    Media::SendCodecRequest sendCodecs;

    Audio::VoiceConnection *rtc = nullptr;
    bool rtcConnected = false;
    Discord::Voice::VideoCodec sendCodec = Discord::Voice::VideoCodec::Unknown;
    bool videoAnnounced = false;

    struct Layer
    {
        int quality = Discord::Voice::FULL_QUALITY;
        Video::CameraSendPlan plan;
        Video::CameraEncoding asked;
        std::optional<int> receivers;
        bool suspended = false;
        Video::PipelineStats encoderStats;
    };
    [[nodiscard]] int layerOf(int quality) const { return quality == layers[Video::CAMERA_SECOND_LAYER].quality ? Video::CAMERA_SECOND_LAYER : Video::CAMERA_FULL_LAYER; }

    std::array<Layer, Video::CAMERA_LAYERS> layers;
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
