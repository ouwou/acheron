#include "Core/Media/AudioOutput.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/Miniaudio.hpp"

#include "Core/Logging.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace Acheron {
namespace Core {
namespace Media {

static constexpr ma_uint32 RingBufferFrames = AudioOutput::SampleRate;

struct AudioOutputState
{
    ma_pcm_rb rb = {};
    std::atomic<bool> rbInit{ false };
};

class MediaAudioDevice
{
public:
    static MediaAudioDevice &instance()
    {
        static MediaAudioDevice device;
        return device;
    }

    bool add(AudioOutput *stream)
    {
        {
            std::lock_guard<std::mutex> lock(mixMutex);
            if (std::find(streams.begin(), streams.end(), stream) == streams.end())
                streams.push_back(stream);
        }

        if (ensureRunning())
            return true;

        remove(stream);
        return false;
    }

    void remove(AudioOutput *stream)
    {
        bool idle;
        {
            // holding this means no callback is midway through reading the stream
            std::lock_guard<std::mutex> lock(mixMutex);
            streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
            idle = streams.empty();
        }

        if (!idle)
            return;

        std::lock_guard<std::mutex> lock(deviceMutex);
        if (deviceRunning) {
            ma_device_stop(&device);
            deviceRunning = false;
        }
    }

    void resetStream(AudioOutput *stream, qint64 basePtsMs)
    {
        std::lock_guard<std::mutex> lock(mixMutex);
        stream->resetLocked(basePtsMs);
    }

    void mix(void *output, uint32_t frameCount)
    {
        auto *out = static_cast<float *>(output);
        const size_t samples = static_cast<size_t>(frameCount) * AudioOutput::Channels;
        std::memset(out, 0, samples * sizeof(float));

        {
            std::lock_guard<std::mutex> lock(mixMutex);
            for (AudioOutput *stream : streams)
                stream->mixInto(out, frameCount, scratch);
        }

        for (size_t i = 0; i < samples; ++i)
            out[i] = qBound(-1.0f, out[i], 1.0f);
    }

private:
    MediaAudioDevice() = default;

    ~MediaAudioDevice()
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        if (deviceInit)
            ma_device_uninit(&device);
    }

    bool ensureRunning()
    {
        // no touch mixMutex and deviceMutex simultaneously
        std::lock_guard<std::mutex> lock(deviceMutex);

        if (!deviceInit) {
            ma_device_config config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            config.playback.channels = AudioOutput::Channels;
            config.sampleRate = AudioOutput::SampleRate;
            config.dataCallback = &MediaAudioDevice::onPlayback;
            config.pUserData = this;
            config.wasapi.noAutoConvertSRC = MA_TRUE;

            if (!Audio::initAudioDevice(&config, &device)) {
                qCWarning(LogVideo) << "failed to init the media playback device";
                return false;
            }
            deviceInit = true;
        }

        if (deviceRunning)
            return true;

        if (ma_device_start(&device) != MA_SUCCESS) {
            qCWarning(LogVideo) << "failed to start the media playback device";
            return false;
        }

        deviceRunning = true;
        return true;
    }

    static void onPlayback(ma_device *device, void *output, const void *input, uint32_t frameCount)
    {
        Q_UNUSED(input);
        if (auto *self = static_cast<MediaAudioDevice *>(device->pUserData))
            self->mix(output, frameCount);
    }

    std::mutex mixMutex;
    std::vector<AudioOutput *> streams;
    std::vector<float> scratch;

    std::mutex deviceMutex;
    ma_device device = {};
    bool deviceInit = false;
    bool deviceRunning = false;
};

AudioOutput::AudioOutput() : state(std::make_unique<AudioOutputState>()) {}

AudioOutput::~AudioOutput()
{
    stop();

    if (state->rbInit.load(std::memory_order_relaxed))
        ma_pcm_rb_uninit(&state->rb);
}

bool AudioOutput::start()
{
    if (running.load(std::memory_order_relaxed))
        return true;

    if (!state->rbInit.load(std::memory_order_acquire)) {
        if (ma_pcm_rb_init(ma_format_f32, Channels, RingBufferFrames, nullptr, nullptr, &state->rb) != MA_SUCCESS) {
            qCWarning(LogVideo) << "failed to allocate the audio ring buffer";
            return false;
        }

        state->rbInit.store(true, std::memory_order_release);
    }

    if (!MediaAudioDevice::instance().add(this))
        return false;

    running.store(true, std::memory_order_relaxed);
    return true;
}

void AudioOutput::stop()
{
    if (!running.load(std::memory_order_relaxed))
        return;

    running.store(false, std::memory_order_relaxed);
    MediaAudioDevice::instance().remove(this);
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
    MediaAudioDevice::instance().resetStream(this, basePtsMs);
}

void AudioOutput::resetLocked(qint64 basePtsMs)
{
    if (state->rbInit.load(std::memory_order_acquire))
        ma_pcm_rb_reset(&state->rb);

    framesPlayed.store(0, std::memory_order_relaxed);
    basePts.store(basePtsMs, std::memory_order_relaxed);
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

void AudioOutput::mixInto(float *output, uint32_t frameCount, std::vector<float> &scratch)
{
    if (!state->rbInit.load(std::memory_order_acquire))
        return;

    const size_t wanted = static_cast<size_t>(frameCount) * Channels;
    if (scratch.size() < wanted)
        scratch.resize(wanted);

    ma_uint32 remaining = frameCount;
    ma_uint32 filled = 0;

    while (remaining > 0) {
        ma_uint32 toRead = remaining;
        void *readPtr = nullptr;
        if (ma_pcm_rb_acquire_read(&state->rb, &toRead, &readPtr) != MA_SUCCESS || toRead == 0)
            break;

        std::memcpy(scratch.data() + static_cast<size_t>(filled) * Channels,
                    readPtr,
                    static_cast<size_t>(toRead) * Channels * sizeof(float));
        ma_pcm_rb_commit_read(&state->rb, toRead);
        filled += toRead;
        remaining -= toRead;
    }

    if (filled == 0)
        return;

    const float gain = muted.load(std::memory_order_relaxed)
                               ? 0.0f
                               : outputVolume.load(std::memory_order_relaxed);
    const size_t samples = static_cast<size_t>(filled) * Channels;
    for (size_t i = 0; i < samples; ++i)
        output[i] += scratch[i] * gain;

    framesPlayed.fetch_add(filled, std::memory_order_relaxed);
}

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
