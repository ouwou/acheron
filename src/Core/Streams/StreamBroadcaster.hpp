#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <functional>
#include <memory>

#include "Core/ProxyConfig.hpp"
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

// What the account may do with a screen share where it is in voice now.
struct ShareTerms
{
    bool allowed = false;
    Video::SharerTier tier;
    // Empty when none is known, which the gateway is told as null.
    QString preferredRegion;
};

// The account's own screen share: asks the gateway for the stream, runs the stream's RTC
// connection as its owner, and feeds it from the capture pipeline. One at a time, in the voice
// channel or call the account is connected to.
class StreamBroadcaster : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        // The gateway has been asked and has not answered yet.
        Starting,
        // The stream exists; its RTC connection or its encoder is not up yet.
        Connecting,
        Live,
        Reconnecting,
    };
    Q_ENUM(State)

    // What has become of the sound a share was started with.
    enum class AudioState {
        // None was asked for, this machine cannot capture any, or the RTC connection is not up.
        Off,
        Capturing,
        // The capture failed, and the share goes on without sound.
        Failed,
    };
    Q_ENUM(AudioState)

    struct Stats
    {
        Video::PipelineStats encoder;
        Discord::Voice::VideoSendStats network;
        // What the encoder is asked for, in bits per second.
        int targetBitrate = 0;
        // As the server counts them; negative until it has said.
        int receivers = -1;
        bool encodingSuspended = false;
        bool captureStalled = false;
        AudioState audio = AudioState::Off;
    };

    using TermsProvider = std::function<ShareTerms()>;

    StreamBroadcaster(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, Snowflake accountId, const ProxyConfig &proxy, TermsProvider terms, QObject *parent = nullptr);
    ~StreamBroadcaster() override;

    void setGatewaySessionId(const QString &sessionId);
    // A new gateway session has forgotten the stream, so nothing is sent for it.
    void reset();

    // Whether a share could be started now: connected to voice, somewhere sharing is allowed.
    [[nodiscard]] bool canShare() const;
    [[nodiscard]] Video::SharerTier tier() const { return terms().tier; }

    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isSharing() const { return currentState != State::Idle; }
    [[nodiscard]] Video::SendQuality quality() const { return sendQuality; }
    // Whether what is being shared, or was last, is a window rather than a screen.
    [[nodiscard]] bool sharesWindow() const { return source.kind == Video::CaptureSource::Kind::Window; }
    [[nodiscard]] Stats stats() const;
    // The capture's latest small picture. Null until there is one.
    [[nodiscard]] QImage preview() const { return latestPreview; }
    // Why the last share ended by itself, in one sentence. Empty once it has been cleared or
    // another share has started.
    [[nodiscard]] QString failure() const { return failureText; }
    void clearFailure();

    // `withAudio` also sends what the shared window's program is playing or, for a screen,
    // everything this machine plays except this client's own sound.
    void start(const Video::CaptureSource &source, const Video::SendQuality &quality, bool withAudio);
    // While live, without reconnecting. A quality the account may not use is not taken.
    void setQuality(const Video::SendQuality &quality);
    void stop();

    static constexpr int START_TIMEOUT_MS = 5000;
    // An encoder that cannot follow a new rate is reopened for it, which starts with a key
    // frame, so rates that change in quick succession (a viewer dragging a window's edge) are
    // gathered into one.
    static constexpr int BITRATE_SETTLE_MS = 2000;
    // How long the server is given to name another codec once the one in use has been withdrawn.
    static constexpr int CODEC_TIMEOUT_MS = 8000;

signals:
    // Also sent when the capture stalls or resumes, when the sound starts being captured or
    // fails, and when a failure is cleared.
    void stateChanged();
    void previewChanged(const QImage &preview);
    // The share ended without being asked to. failure() has the same sentence.
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
    void onRtcStateChanged(Acheron::Discord::Voice::VoiceClient::State state);
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
    void end(bool tellGateway);
    void fail(const QString &reason, bool tellGateway = true);

    void openRtc();
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
    Snowflake accountId;
    ProxyConfig proxy;
    TermsProvider terms;
    QString gatewaySessionId;

    State currentState = State::Idle;
    QString failureText;
    Discord::StreamKey streamKey;
    Video::CaptureSource source;
    bool audioWanted = false;
    Video::SendQuality sendQuality;
    QString preferredRegion;
    // What this share offers to send with: the codecs this machine encodes, less those whose
    // encoder would have added black to a size the share has had. One that is withdrawn stays
    // so until the share ends, or a window dragged by its edge would have the server change
    // the codec back and forth.
    QList<Discord::Voice::VideoCodec> offeredCodecs;
    quint64 encoderProbeAttempt = 0;
    bool createSent = false;
    // A stream the gateway did not answer for in time. If it turns up after all it is ended.
    QString abandonedStreamKey;

    StreamConnection *rtc = nullptr;
    bool rtcConnected = false;
    // The codec the server last named, and the one the pipeline was started with.
    Discord::Voice::VideoCodec serverCodec = Discord::Voice::VideoCodec::Unknown;
    Discord::Voice::VideoCodec sendCodec = Discord::Voice::VideoCodec::Unknown;
    bool videoAnnounced = false;
    Discord::Voice::LocalVideoDescription announced;
    int receivers = -1;

    // The sound that goes with the share, for as long as one RTC connection lasts: captured on
    // a thread of the capture's own, encoded and sent on the connection's.
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
