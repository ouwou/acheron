#pragma once

#include <QtGlobal>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

struct ma_device;

namespace Acheron {
namespace Core {
namespace Media {

struct AudioOutputState;

class AudioOutput
{
public:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;

    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const { return running.load(std::memory_order_relaxed); }

    [[nodiscard]] int writableFrames() const;
    int write(const float *interleaved, int frameCount);

    [[nodiscard]] int bufferedFrames() const;

    void reset(qint64 basePtsMs);

    void setVolume(float volume);
    [[nodiscard]] float volume() const { return outputVolume.load(std::memory_order_relaxed); }
    void setMuted(bool value);
    [[nodiscard]] bool isMuted() const { return muted.load(std::memory_order_relaxed); }

    [[nodiscard]] qint64 clockMs() const;

private:
    void renderFrames(void *output, uint32_t frameCount);

    friend void OnVideoPlayback(ma_device *, void *, const void *, uint32_t);

    std::unique_ptr<AudioOutputState> state;
    mutable std::mutex controlMutex;

    std::atomic<float> outputVolume{ 1.0f };
    std::atomic<bool> muted{ false };
    std::atomic<bool> running{ false };
    std::atomic<qint64> framesPlayed{ 0 };
    std::atomic<qint64> basePts{ 0 };
};

} // namespace Media
} // namespace Core
} // namespace Acheron
