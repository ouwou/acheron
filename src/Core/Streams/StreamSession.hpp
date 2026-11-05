#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QTimer>

#include <memory>
#include <optional>

#include "Core/Media/DecodeDemand.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamConnection.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Discord/StreamKey.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

namespace Audio {
class VoiceConnection;
class VoiceManager;
} // namespace Audio

class StreamSession : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Connecting,
        Active,
        Paused,
        Reconnecting,
        Ended,
        Failed,
        Full,
    };
    Q_ENUM(State)

    struct Stats
    {
        Discord::Voice::VideoReceiveStats network;
        Media::VideoDecoder::Stats decoder;
        double bitrateKbps = 0;
        double framesPerSecond = 0;
        double pixelShareAskedFor = 1;
    };

    StreamSession(const Discord::StreamKey &key, Audio::VoiceManager *voice, QObject *parent = nullptr);
    ~StreamSession() override;

    [[nodiscard]] const Discord::StreamKey &key() const { return streamKey; }
    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] bool isOver() const { return currentState == State::Ended || currentState == State::Failed || currentState == State::Full; }
    [[nodiscard]] Stats stats() const { return latestStats; }
    [[nodiscard]] float volume() const { return audioVolume; }

    void apply(const StreamRecord &record);
    void end(StreamEndReason reason);

    void setSink(std::shared_ptr<Media::VideoFrameSink> sink);
    void setSurface(int pixelCount, bool visible);
    void setVolume(float volume);

    static constexpr int SURFACE_DEBOUNCE_MS = 500;

signals:
    void stateChanged();
    void pingRequested();

private slots:
    void onRtcOpened(Acheron::Core::Audio::VoiceConnection *connection);
    void onRtcConnected();
    void onRtcClosed();
    void onRtcReconnecting();
    void onRtcFailed();
    void onRemoteVideoChanged(Snowflake userId, bool active, std::optional<QSize> maxSize);
    void onVideoStats(const Discord::Voice::VideoReceiveStats &stats);

private:
    void setState(State state);
    void pushWants();

    Discord::StreamKey streamKey;
    Audio::VoiceManager *voice;
    quint32 audioSource;

    State currentState = State::Connecting;
    bool paused = false;
    qint64 announcedPixels = 0;

    StreamConnection *rtc;
    std::unique_ptr<Media::VideoDecoder> decoder;
    std::shared_ptr<Media::VideoFrameSink> frameSink;

    QTimer surfaceDebounce;
    int surfacePixels = 0;
    bool surfaceVisible = false;
    float audioVolume = 1.0f;

    Media::DecodeDemand demand;
    QElapsedTimer demandClock;
    // what arrives shrinks once less is asked for, so only the largest seen measures the whole picture
    qint64 fullPicturePixels = 0;

    Stats latestStats;
    QElapsedTimer statsClock;
    quint64 statsBytes = 0;
    quint64 statsDecoded = 0;
};

} // namespace Core
} // namespace Acheron
