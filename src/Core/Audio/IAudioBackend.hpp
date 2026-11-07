#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>

namespace Acheron {
namespace Core {
namespace Audio {

inline constexpr uint8_t OPUS_SILENCE[] = { 0xF8, 0xFF, 0xFE };

[[nodiscard]] inline bool isOpusSilence(std::span<const uint8_t> frame)
{
    return std::equal(frame.begin(), frame.end(), std::begin(OPUS_SILENCE), std::end(OPUS_SILENCE));
}

[[nodiscard]] inline bool isOpusSilence(const QByteArray &frame)
{
    return isOpusSilence(std::span(reinterpret_cast<const uint8_t *>(frame.constData()), static_cast<size_t>(frame.size())));
}

inline constexpr int AUDIO_SAMPLE_RATE = 48000;
inline constexpr int AUDIO_CHANNELS = 2;
inline constexpr int AUDIO_SAMPLE_BYTES = 2;
inline constexpr int AUDIO_FRAME_DURATION_MS = 20;
inline constexpr int AUDIO_FRAME_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_FRAME_DURATION_MS / 1000; // 960
inline constexpr int AUDIO_FRAME_SIZE = AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES; // 3840 bytes

[[nodiscard]] inline float rmsOf(const int16_t *samples, int count)
{
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += static_cast<double>(samples[i]) * samples[i];
    return count > 0 ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
}

struct AudioDeviceInfo
{
    QByteArray id;
    QString description;
    bool isDefault = false;
};

class IAudioBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~IAudioBackend() override = default;

    static std::unique_ptr<IAudioBackend> create(QObject *parent = nullptr);

    virtual QList<AudioDeviceInfo> availableInputDevices() const = 0;
    virtual QList<AudioDeviceInfo> availableOutputDevices() const = 0;

    [[nodiscard]] virtual QByteArray currentInputDevice() const = 0;
    [[nodiscard]] virtual QByteArray currentOutputDevice() const = 0;
    virtual void setInputDevice(const QByteArray &deviceId) = 0;
    virtual void setOutputDevice(const QByteArray &deviceId) = 0;

    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual bool startPlayback() = 0;
    virtual void stopPlayback() = 0;

    [[nodiscard]] virtual bool isCapturing() const = 0;
    [[nodiscard]] virtual bool isPlaying() const = 0;

    [[nodiscard]] virtual int nativeCaptureChannels() const = 0;

    virtual void setInputGain(float gain) = 0;
    virtual void setOutputVolume(float volume) = 0;

    virtual bool pushPlaybackFrame(const int16_t *frame) = 0;

signals:
    void audioCaptured(const QByteArray &pcmData);
    void devicesChanged(const QList<AudioDeviceInfo> &inputs, const QList<AudioDeviceInfo> &outputs);
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
