#include "Core/Video/AudioOutput.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#  include "Core/Audio/AudioBackends.hpp"
#  include "Core/Audio/Miniaudio.hpp"

#  include "Core/Logging.hpp"

#  include <cstring>

namespace Acheron {
namespace Core {
namespace Video {

static constexpr ma_uint32 RingBufferFrames = AudioOutput::SampleRate;

struct AudioOutputState
{
    ma_device device = {};
    ma_pcm_rb rb = {};
    bool deviceInit = false;
    std::atomic<bool> rbInit{ false };
};

void OnVideoPlayback(ma_device *device, void *output, const void *input, uint32_t frameCount)
{
    Q_UNUSED(input);
    auto *self = static_cast<AudioOutput *>(device->pUserData);
    if (self)
        self->renderFrames(output, frameCount);
}

AudioOutput::AudioOutput() : state(std::make_unique<AudioOutputState>()) {}

AudioOutput::~AudioOutput()
{
    stop();

    if (state->deviceInit)
        ma_device_uninit(&state->device);
    if (state->rbInit.load(std::memory_order_relaxed))
        ma_pcm_rb_uninit(&state->rb);
}

bool AudioOutput::start()
{
    std::lock_guard<std::mutex> lock(controlMutex);

    if (running.load(std::memory_order_relaxed))
        return true;

    if (!state->rbInit.load(std::memory_order_acquire)) {
        if (ma_pcm_rb_init(ma_format_f32, Channels, RingBufferFrames, nullptr, nullptr, &state->rb) != MA_SUCCESS) {
            qCWarning(LogVideo) << "failed to allocate the audio ring buffer";
            return false;
        }

        state->rbInit.store(true, std::memory_order_release);
    }

    if (!state->deviceInit) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = Channels;
        config.sampleRate = SampleRate;
        config.dataCallback = OnVideoPlayback;
        config.pUserData = this;
        config.wasapi.noAutoConvertSRC = MA_TRUE;

        if (!Audio::initAudioDevice(&config, &state->device)) {
            qCWarning(LogVideo) << "failed to init the video playback device";
            return false;
        }
        state->deviceInit = true;
    }

    if (ma_device_start(&state->device) != MA_SUCCESS) {
        qCWarning(LogVideo) << "failed to start the video playback device";
        return false;
    }

    running.store(true, std::memory_order_relaxed);
    return true;
}

void AudioOutput::stop()
{
    std::lock_guard<std::mutex> lock(controlMutex);

    if (!running.load(std::memory_order_relaxed))
        return;

    ma_device_stop(&state->device);
    running.store(false, std::memory_order_relaxed);
}

int AudioOutput::writableFrames() const
{
    if (!state->rbInit.load(std::memory_order_acquire))
        return 0;
    return static_cast<int>(ma_pcm_rb_available_write(&state->rb));
}

int AudioOutput::bufferedFrames() const
{
    if (!state->rbInit.load(std::memory_order_acquire))
        return 0;
    return static_cast<int>(ma_pcm_rb_available_read(&state->rb));
}

int AudioOutput::write(const float *interleaved, int frameCount)
{
    if (!state->rbInit.load(std::memory_order_acquire) || !interleaved || frameCount <= 0)
        return 0;

    int written = 0;
    while (written < frameCount) {
        ma_uint32 toWrite = static_cast<ma_uint32>(frameCount - written);
        void *writePtr = nullptr;
        if (ma_pcm_rb_acquire_write(&state->rb, &toWrite, &writePtr) != MA_SUCCESS || toWrite == 0)
            break;

        std::memcpy(writePtr,
                    interleaved + static_cast<size_t>(written) * Channels,
                    static_cast<size_t>(toWrite) * Channels * sizeof(float));
        ma_pcm_rb_commit_write(&state->rb, toWrite);
        written += static_cast<int>(toWrite);
    }
    return written;
}

void AudioOutput::reset(qint64 basePtsMs)
{
    std::lock_guard<std::mutex> lock(controlMutex);

    if (!state->rbInit.load(std::memory_order_acquire)) {
        framesPlayed.store(0, std::memory_order_relaxed);
        basePts.store(basePtsMs, std::memory_order_relaxed);
        return;
    }

    const bool wasRunning = running.load(std::memory_order_relaxed);
    if (wasRunning)
        ma_device_stop(&state->device);

    ma_pcm_rb_reset(&state->rb);
    framesPlayed.store(0, std::memory_order_relaxed);
    basePts.store(basePtsMs, std::memory_order_relaxed);

    if (wasRunning && ma_device_start(&state->device) != MA_SUCCESS) {
        qCWarning(LogVideo) << "failed to restart the playback device after a seek";
        running.store(false, std::memory_order_relaxed);
    }
}

void AudioOutput::setVolume(float volume)
{
    outputVolume.store(qBound(0.0f, volume, 1.0f), std::memory_order_relaxed);
}

void AudioOutput::setMuted(bool value)
{
    muted.store(value, std::memory_order_relaxed);
}

qint64 AudioOutput::clockMs() const
{
    const qint64 played = framesPlayed.load(std::memory_order_relaxed);
    return basePts.load(std::memory_order_relaxed) + (played * 1000) / SampleRate;
}

void AudioOutput::renderFrames(void *output, uint32_t frameCount)
{
    auto *out = static_cast<float *>(output);
    ma_uint32 remaining = frameCount;
    ma_uint32 filled = 0;

    while (remaining > 0) {
        ma_uint32 toRead = remaining;
        void *readPtr = nullptr;
        if (ma_pcm_rb_acquire_read(&state->rb, &toRead, &readPtr) != MA_SUCCESS || toRead == 0)
            break;

        std::memcpy(out + static_cast<size_t>(filled) * Channels,
                    readPtr,
                    static_cast<size_t>(toRead) * Channels * sizeof(float));
        ma_pcm_rb_commit_read(&state->rb, toRead);
        filled += toRead;
        remaining -= toRead;
    }

    const float gain = muted.load(std::memory_order_relaxed)
                               ? 0.0f
                               : outputVolume.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
        const size_t samples = static_cast<size_t>(filled) * Channels;
        for (size_t i = 0; i < samples; ++i)
            out[i] *= gain;
    }

    if (remaining > 0)
        std::memset(out + static_cast<size_t>(filled) * Channels,
                    0,
                    static_cast<size_t>(remaining) * Channels * sizeof(float));

    if (filled > 0)
        framesPlayed.fetch_add(filled, std::memory_order_relaxed);
}

} // namespace Video
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
