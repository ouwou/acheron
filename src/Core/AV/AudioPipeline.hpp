#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QVector>

#include <memory>
#include <unordered_map>

#include "Core/Snowflake.hpp"

class QTimer;

namespace Acheron {
namespace Core {
namespace AV {

class IAudioBackend;
class OpusEncoder;
class OpusDecoder;
class JitterBuffer;

struct SpeakerState
{
    std::unique_ptr<OpusDecoder> decoder;
    std::unique_ptr<JitterBuffer> jitterBuffer;
    QVector<QByteArray> pendingFrames; // overflow from multi-frame Opus packets
};

class AudioPipeline : public QObject
{
    Q_OBJECT
public:
    explicit AudioPipeline(QObject *parent = nullptr);
    ~AudioPipeline() override;

public slots:
    void start(IAudioBackend *backend, bool capturing);
    void stop();

    void startCapture();
    void stopCapture();

    void onAudioReceived(quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData);

    void setSsrcUserId(quint32 ssrc, Snowflake userId);
    void removeUser(Snowflake userId);
    void setDeafened(bool deafened);
    void setUserVolume(Snowflake userId, float volume);
    void setInputDevice(const QByteArray &deviceId);
    void setOutputDevice(const QByteArray &deviceId);
    void setInputGain(float gain);
    void setOutputVolume(float volume);
    void setVadThreshold(float threshold);

signals:
    void encodedAudioReady(const QByteArray &opusData);
    void speakingChanged(bool speaking);
    void audioLevelChanged(float rms);
    void userAudioLevelChanged(Snowflake userId, float rms);

private slots:
    void onAudioCaptured(const QByteArray &pcmData);
    void onMixTick();

private:
    bool detectVoiceActivity(const QByteArray &pcmFrame, float &outRms) const;
    void sendTrailingSilence();
    static float computeRms(const int16_t *samples, int count);

    IAudioBackend *audioBackend = nullptr;
    QTimer *mixTimer = nullptr;
    std::unique_ptr<OpusEncoder> encoder;
    std::unordered_map<quint32, SpeakerState> speakers;

    QHash<quint32, Snowflake> ssrcToUser;
    QHash<Snowflake, float> userVolumes;

    bool deafened = false;
    bool isSpeaking = false;
    float vadThreshold = 100.0f;
    int vadHoldoffFrames = 25;
    int vadHoldoffCounter = 0;

    QElapsedTimer rmsThrottleTimer;
    QElapsedTimer userRmsThrottleTimer;
    QHash<Snowflake, float> pendingUserRms;

    static constexpr int TRAILING_SILENCE_FRAMES = 5;
    static constexpr qint64 RMS_EMIT_INTERVAL_MS = 60;
};

} // namespace AV
} // namespace Core
} // namespace Acheron
