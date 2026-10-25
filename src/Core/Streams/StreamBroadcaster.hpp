#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

#include "Core/GatewayNotice.hpp"
#include "Core/Media/CodecSupport.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamConnection.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Core/Video/CaptureSources.hpp"
#include "Core/Video/ScreenPipeline.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Core/Video/SendRateController.hpp"
#include "Discord/StreamKey.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Discord {
class Client;
}

namespace Core {

class StreamAudio;
class StreamPreviewUploader;

namespace Audio {
class LoopbackCapture;
class VoiceConnection;
class VoiceManager;
} // namespace Audio

struct ShareTerms
{
    bool allowed = false;
    Video::SharerTier tier;
    // empty is sent to the gateway as null
    QString preferredRegion;
};

class StreamBroadcaster : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Starting,
        Connecting,
        Live,
        Reconnecting,
    };
    Q_ENUM(State)

    enum class AudioState {
        Off,
        Capturing,
        Failed,
    };
    Q_ENUM(AudioState)

    struct Stats
    {
        Video::PipelineStats encoder;
        Discord::Voice::VideoSendStats network;
        int targetBitrateBps = 0;
        std::optional<int> receivers;
        bool encodingSuspended = false;
        bool captureStalled = false;
        AudioState audio = AudioState::Off;
    };

    using TermsProvider = std::function<ShareTerms()>;

    StreamBroadcaster(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, TermsProvider terms, QObject *parent = nullptr);
    ~StreamBroadcaster() override;

    void reset();

    [[nodiscard]] bool canShare() const;
    [[nodiscard]] Video::SharerTier tier() const { return terms().tier; }

    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isSharing() const { return currentState != State::Idle; }
    [[nodiscard]] Video::SendQuality quality() const { return sendQuality; }
    [[nodiscard]] bool sharesWindow() const { return source.kind == Video::CaptureSource::Kind::Window; }
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] QImage preview() const { return latestPreview; }
    [[nodiscard]] QString failure() const { return failureText; }
    void clearFailure();

    void start(const Video::CaptureSource &source, const Video::SendQuality &quality, bool withAudio);
    void setQuality(const Video::SendQuality &quality);
    void stop();

    static constexpr int START_TIMEOUT_MS = 5000;
    // a rate change can reopen the encoder and cost a key frame, so changes in quick succession are
    // gathered into one
    static constexpr int BITRATE_SETTLE_MS = 2000;

signals:
    void stateChanged();
    void previewChanged(const QImage &preview);
    void failed(const QString &reason);

private slots:
    void onStreamChanged(const QString &streamKey);
    void onStreamEnded(const QString &streamKey, Acheron::Core::StreamEndReason reason);
    void onVoiceConnected();
    void onVoiceStateChanged();
    void onVoiceDisconnected();
    void onStartTimedOut();
    void onCodecTimedOut();

    void onRtcOpened(Acheron::Core::Audio::VoiceConnection *connection);
    void onRtcClosed();
    void onRtcReconnecting();
    void onRtcFailed(Acheron::Core::StreamConnection::Failure failure);
    void onRtcConnected();
    void onSendCodecChanged(Acheron::Discord::Voice::VideoCodec codec);
    void onSendStats(const Acheron::Discord::Voice::VideoSendStats &stats);
    void onKeyframeRequested();
    void onReceiversChanged(int receivers);

private:
    void onWantsReceived(int pixelCount);
    void onPipelineStarted(const QSize &encodedSize, int framerate);
    void onPreviewReady(const QImage &preview);
    void onCaptureStalled(bool stalled);
    void onSourceClosed();
    void onPipelineFailed(const QString &reason);
    void onPipelineStats(const Video::PipelineStats &stats);
    void onSoundCaptureFailed(const QString &reason);

    void setState(State state);
    void sendCreate(const QList<Discord::Voice::VideoCodec> &codecs);
    void end(GatewayNotice notice);
    void fail(const QString &reason, GatewayNotice notice = GatewayNotice::Send);

    void openRtc();
    [[nodiscard]] bool soundIsOneProgram() const;
    [[nodiscard]] bool sendsSound() const;
    void startSoundCapture();
    void useServerCodec();

    void withdrawCodecsUnfitFor(const QSize &size);
    void startPipeline();
    void stopPipeline();
    void sendFramesToRtc();
    void announceVideo();
    void describeVideoIfChanged();
    [[nodiscard]] Discord::Voice::LocalVideoDescription videoDescription() const;
    void refreshLimits();
    void applyBitrateOnceSettled();
    void applyBitrate();
    void tellBitrate();
    void applySuspension();

    Discord::Client *client;
    StreamManager *streams;
    Audio::VoiceManager *voice;
    TermsProvider terms;

    State currentState = State::Idle;
    QString failureText;
    Discord::StreamKey streamKey;
    Video::CaptureSource source;
    bool audioWanted = false;
    Video::SendQuality sendQuality;
    QString preferredRegion;
    // a withdrawn codec stays withdrawn for the share, or dragging a window's edge would flip the codec
    // back and forth
    QList<Discord::Voice::VideoCodec> offeredCodecs;
    Media::SendCodecRequest sendCodecs;
    bool createSent = false;
    QString abandonedStreamKey;

    StreamConnection *rtc = nullptr;
    Discord::Voice::VideoCodec serverCodec = Discord::Voice::VideoCodec::Unknown;
    Discord::Voice::VideoCodec sendCodec = Discord::Voice::VideoCodec::Unknown;
    bool videoAnnounced = false;
    Discord::Voice::LocalVideoDescription announced;
    std::optional<int> receivers;

    // lives as long as one RTC connection
    std::unique_ptr<StreamAudio> streamAudio;
    std::unique_ptr<Audio::LoopbackCapture> soundCapture;
    bool soundCaptureFailed = false;

    std::unique_ptr<Video::ScreenPipeline> pipeline;
    QMetaObject::Connection framesToRtc;
    bool pipelineStarted = false;
    QSize encodedSize;
    int encodedFramerate = 0;
    bool encodingSuspended = false;
    bool captureStalled = false;

    Video::ViewerSize viewerSize;
    Video::BitrateLimits limits;
    Video::SendRateController rate;
    QElapsedTimer rateClock;

    StreamPreviewUploader *previewUploader;
    QImage latestPreview;
    Video::PipelineStats encoderStats;
    Discord::Voice::VideoSendStats networkStats;

    QTimer startTimeout;
    QTimer codecTimeout;
    QTimer bitrateSettle;
    QElapsedTimer sinceBitrateApplied;
    int appliedBitrate = 0;
};

} // namespace Core
} // namespace Acheron
