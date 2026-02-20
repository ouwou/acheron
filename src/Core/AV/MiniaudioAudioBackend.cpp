#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include "MiniaudioAudioBackend.hpp"

#include "Core/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Acheron {
namespace Core {
namespace AV {

static constexpr ma_uint32 PLAYBACK_RB_FRAMES = AUDIO_FRAME_SAMPLES * 8;

struct MiniaudioState
{
    ma_context context = {};
    ma_device captureDevice = {};
    ma_device playbackDevice = {};
    ma_pcm_rb playbackRB = {};
    bool contextInit = false;
    bool captureDeviceInit = false;
    bool playbackDeviceInit = false;
    bool playbackRBInit = false;
};

void OnCapture(ma_device *pDevice, void *, const void *pInput, ma_uint32 frameCount)
{
    static_cast<MiniaudioAudioBackend *>(pDevice->pUserData)->handleCapturedFrames(pInput, frameCount);
}

void OnPlayback(ma_device *pDevice, void *pOutput, const void *, ma_uint32 frameCount)
{
    static_cast<MiniaudioAudioBackend *>(pDevice->pUserData)->handlePlaybackFrames(pOutput, frameCount);
}

static QByteArray SerializeDeviceId(const ma_device_id &id)
{
    return QByteArray(reinterpret_cast<const char *>(&id), sizeof(ma_device_id));
}

static bool DeserializeDeviceId(const QByteArray &bytes, ma_device_id &id)
{
    if (bytes.size() != sizeof(ma_device_id))
        return false;
    std::memcpy(&id, bytes.constData(), sizeof(ma_device_id));
    return true;
}

static QList<AudioDeviceInfo> EnumerateDevices(ma_context *ctx, ma_device_type type)
{
    QList<AudioDeviceInfo> result;

    ma_device_info *pPlayback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info *pCapture = nullptr;
    ma_uint32 captureCount = 0;

    if (ma_context_get_devices(ctx, &pPlayback, &playbackCount, &pCapture, &captureCount) != MA_SUCCESS)
        return result;

    ma_device_info *infos = (type == ma_device_type_capture) ? pCapture : pPlayback;
    ma_uint32 count = (type == ma_device_type_capture) ? captureCount : playbackCount;

    for (ma_uint32 i = 0; i < count; i++) {
        AudioDeviceInfo info;
        info.id = SerializeDeviceId(infos[i].id);
        info.description = QString::fromUtf8(infos[i].name);
        info.isDefault = infos[i].isDefault != 0;
        result.append(info);
    }

    return result;
}

MiniaudioAudioBackend::MiniaudioAudioBackend(QObject *parent)
    : IAudioBackend(parent),
      ma(std::make_unique<MiniaudioState>())
{
    if (ma_context_init(NULL, 0, NULL, &ma->context) != MA_SUCCESS)
        qCWarning(LogVoice) << "Failed to initialize miniaudio context";
    else
        ma->contextInit = true;

    if (ma_pcm_rb_init(ma_format_s16, AUDIO_CHANNELS, PLAYBACK_RB_FRAMES, NULL, NULL, &ma->playbackRB) != MA_SUCCESS)
        qCWarning(LogVoice) << "Failed to init playback ring buffer";
    else
        ma->playbackRBInit = true;
}

MiniaudioAudioBackend::~MiniaudioAudioBackend()
{
    stopCapture();
    stopPlayback();

    if (ma->playbackRBInit)
        ma_pcm_rb_uninit(&ma->playbackRB);

    if (ma->contextInit)
        ma_context_uninit(&ma->context);
}

QList<AudioDeviceInfo> MiniaudioAudioBackend::availableInputDevices() const
{
    if (!ma->contextInit)
        return {};
    return EnumerateDevices(&ma->context, ma_device_type_capture);
}

QList<AudioDeviceInfo> MiniaudioAudioBackend::availableOutputDevices() const
{
    if (!ma->contextInit)
        return {};
    return EnumerateDevices(&ma->context, ma_device_type_playback);
}

void MiniaudioAudioBackend::setInputDevice(const QByteArray &deviceId)
{
    if (deviceId == selectedInputId)
        return;

    selectedInputId = deviceId;
    qCInfo(LogVoice) << "Input device changed";

    if (ma->captureDeviceInit) {
        stopCapture();
        startCapture();
    }
}

void MiniaudioAudioBackend::setOutputDevice(const QByteArray &deviceId)
{
    if (deviceId == selectedOutputId)
        return;

    selectedOutputId = deviceId;
    qCInfo(LogVoice) << "Output device changed";

    if (ma->playbackDeviceInit) {
        stopPlayback();
        startPlayback();
    }
}

bool MiniaudioAudioBackend::startCapture()
{
    if (ma->captureDeviceInit)
        return true;

    if (!ma->contextInit)
        return false;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = AUDIO_CHANNELS;
    config.sampleRate = AUDIO_SAMPLE_RATE;
    config.dataCallback = OnCapture;
    config.pUserData = this;
    config.periodSizeInFrames = AUDIO_FRAME_SAMPLES;

    ma_device_id deviceId;
    if (!selectedInputId.isEmpty() && DeserializeDeviceId(selectedInputId, deviceId))
        config.capture.pDeviceID = &deviceId;

    if (ma_device_init(&ma->context, &config, &ma->captureDevice) != MA_SUCCESS) {
        qCWarning(LogVoice) << "Failed to init miniaudio capture device";
        return false;
    }

    captureBuffer.clear();

    if (ma_device_start(&ma->captureDevice) != MA_SUCCESS) {
        qCWarning(LogVoice) << "Failed to start miniaudio capture device";
        ma_device_uninit(&ma->captureDevice);
        return false;
    }

    ma->captureDeviceInit = true;
    selectedInputId = SerializeDeviceId(ma->captureDevice.capture.id);
    qCInfo(LogVoice) << "Miniaudio capture started:" << ma->captureDevice.capture.name;
    return true;
}

void MiniaudioAudioBackend::stopCapture()
{
    if (!ma->captureDeviceInit)
        return;

    // ma_device_uninit stops the device and waits for callbacks to finish
    ma_device_uninit(&ma->captureDevice);
    ma->captureDeviceInit = false;
    captureBuffer.clear();

    qCInfo(LogVoice) << "Miniaudio capture stopped";
}

bool MiniaudioAudioBackend::startPlayback()
{
    if (ma->playbackDeviceInit)
        return true;

    if (!ma->contextInit || !ma->playbackRBInit)
        return false;

    ma_pcm_rb_reset(&ma->playbackRB);

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = AUDIO_CHANNELS;
    config.sampleRate = AUDIO_SAMPLE_RATE;
    config.dataCallback = OnPlayback;
    config.pUserData = this;
    config.periodSizeInFrames = AUDIO_FRAME_SAMPLES;

    ma_device_id deviceId;
    if (!selectedOutputId.isEmpty() && DeserializeDeviceId(selectedOutputId, deviceId))
        config.playback.pDeviceID = &deviceId;

    if (ma_device_init(&ma->context, &config, &ma->playbackDevice) != MA_SUCCESS) {
        qCWarning(LogVoice) << "Failed to init miniaudio playback device";
        return false;
    }

    if (ma_device_start(&ma->playbackDevice) != MA_SUCCESS) {
        qCWarning(LogVoice) << "Failed to start miniaudio playback device";
        ma_device_uninit(&ma->playbackDevice);
        return false;
    }

    ma->playbackDeviceInit = true;
    selectedOutputId = SerializeDeviceId(ma->playbackDevice.playback.id);
    qCInfo(LogVoice) << "Miniaudio playback started:" << ma->playbackDevice.playback.name;
    return true;
}

void MiniaudioAudioBackend::stopPlayback()
{
    if (!ma->playbackDeviceInit)
        return;

    // ma_device_uninit stops the device and waits for callbacks to finish
    ma_device_uninit(&ma->playbackDevice);
    ma->playbackDeviceInit = false;

    qCInfo(LogVoice) << "Miniaudio playback stopped";
}

bool MiniaudioAudioBackend::isCapturing() const
{
    return ma->captureDeviceInit;
}

bool MiniaudioAudioBackend::isPlaying() const
{
    return ma->playbackDeviceInit;
}

void MiniaudioAudioBackend::setInputGain(float gain)
{
    inputGain.store(gain, std::memory_order_relaxed);
}

void MiniaudioAudioBackend::setOutputVolume(float volume)
{
    outputVolume.store(volume, std::memory_order_relaxed);
}

bool MiniaudioAudioBackend::pushPlaybackFrame(const int16_t *frame)
{
    if (!ma->playbackRBInit)
        return false;

    if (ma_pcm_rb_available_write(&ma->playbackRB) < AUDIO_FRAME_SAMPLES)
        return false;

    ma_uint32 written = 0;
    while (written < static_cast<ma_uint32>(AUDIO_FRAME_SAMPLES)) {
        ma_uint32 toWrite = AUDIO_FRAME_SAMPLES - written;
        void *writePtr;
        if (ma_pcm_rb_acquire_write(&ma->playbackRB, &toWrite, &writePtr) != MA_SUCCESS || toWrite == 0)
            return false;
        std::memcpy(writePtr, frame + written * AUDIO_CHANNELS, toWrite * AUDIO_CHANNELS * sizeof(int16_t));
        ma_pcm_rb_commit_write(&ma->playbackRB, toWrite);
        written += toWrite;
    }
    return true;
}

void MiniaudioAudioBackend::handleCapturedFrames(const void *input, unsigned int frameCount)
{
    if (!input)
        return;

    int bytes = static_cast<int>(frameCount) * AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES;
    captureBuffer.append(static_cast<const char *>(input), bytes);

    // Drop excess data to prevent falling behind
    if (captureBuffer.size() > AUDIO_FRAME_SIZE * 2) {
        int keep = captureBuffer.size() % AUDIO_FRAME_SIZE + AUDIO_FRAME_SIZE;
        captureBuffer.remove(0, captureBuffer.size() - keep);
    }

    float gain = inputGain.load(std::memory_order_relaxed);

    while (captureBuffer.size() >= AUDIO_FRAME_SIZE) {
        QByteArray frame = captureBuffer.left(AUDIO_FRAME_SIZE);
        captureBuffer.remove(0, AUDIO_FRAME_SIZE);

        if (gain != 1.0f) {
            auto *samples = reinterpret_cast<int16_t *>(frame.data());
            int count = frame.size() / static_cast<int>(sizeof(int16_t));
            for (int i = 0; i < count; i++) {
                int32_t val = static_cast<int32_t>(std::lround(samples[i] * gain));
                samples[i] = static_cast<int16_t>(std::clamp(val,
                                                             static_cast<int32_t>(INT16_MIN),
                                                             static_cast<int32_t>(INT16_MAX)));
            }
        }

        emit audioCaptured(frame);
    }
}

void MiniaudioAudioBackend::handlePlaybackFrames(void *output, unsigned int frameCount)
{
    auto *out = static_cast<int16_t *>(output);
    ma_uint32 remaining = frameCount;

    while (remaining > 0) {
        ma_uint32 toRead = remaining;
        void *readPtr;
        if (ma_pcm_rb_acquire_read(&ma->playbackRB, &toRead, &readPtr) != MA_SUCCESS || toRead == 0) {
            std::memset(out, 0, remaining * AUDIO_CHANNELS * sizeof(int16_t));
            break;
        }
        std::memcpy(out, readPtr, toRead * AUDIO_CHANNELS * sizeof(int16_t));
        ma_pcm_rb_commit_read(&ma->playbackRB, toRead);
        out += toRead * AUDIO_CHANNELS;
        remaining -= toRead;
    }

    float vol = outputVolume.load(std::memory_order_relaxed);
    if (vol != 1.0f) {
        auto *samples = static_cast<int16_t *>(output);
        int count = static_cast<int>(frameCount) * AUDIO_CHANNELS;
        for (int i = 0; i < count; i++) {
            int32_t val = static_cast<int32_t>(std::lround(samples[i] * vol));
            samples[i] = static_cast<int16_t>(std::clamp(val,
                                                         static_cast<int32_t>(INT16_MIN),
                                                         static_cast<int32_t>(INT16_MAX)));
        }
    }
}

} // namespace AV
} // namespace Core
} // namespace Acheron
