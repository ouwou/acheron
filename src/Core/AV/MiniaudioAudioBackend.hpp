#pragma once

#include "IAudioBackend.hpp"

#include <atomic>
#include <memory>

struct ma_device;

namespace Acheron {
namespace Core {
namespace AV {

struct MiniaudioState;

class MiniaudioAudioBackend : public IAudioBackend
{
    Q_OBJECT
public:
    explicit MiniaudioAudioBackend(QObject *parent = nullptr);
    ~MiniaudioAudioBackend() override;

    QList<AudioDeviceInfo> availableInputDevices() const override;
    QList<AudioDeviceInfo> availableOutputDevices() const override;

    [[nodiscard]] QByteArray currentInputDevice() const override { return selectedInputId; }
    [[nodiscard]] QByteArray currentOutputDevice() const override { return selectedOutputId; }
    void setInputDevice(const QByteArray &deviceId) override;
    void setOutputDevice(const QByteArray &deviceId) override;

    bool startCapture() override;
    void stopCapture() override;
    bool startPlayback() override;
    void stopPlayback() override;

    [[nodiscard]] bool isCapturing() const override;
    [[nodiscard]] bool isPlaying() const override;

    void setInputGain(float gain) override;
    void setOutputVolume(float volume) override;

    bool pushPlaybackFrame(const int16_t *frame) override;

private:
    // audio thread callbacks
    void handleCapturedFrames(const void *input, unsigned int frameCount);
    void handlePlaybackFrames(void *output, unsigned int frameCount);

    friend void OnCapture(ma_device *, void *, const void *, uint32_t);
    friend void OnPlayback(ma_device *, void *, const void *, uint32_t);

private:
    std::unique_ptr<MiniaudioState> ma;

    QByteArray captureBuffer;

    QByteArray selectedInputId;
    QByteArray selectedOutputId;

    std::atomic<float> inputGain{ 1.0f };
    std::atomic<float> outputVolume{ 1.0f };
};

} // namespace AV
} // namespace Core
} // namespace Acheron
