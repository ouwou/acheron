#include "Core/Audio/PlayedSound.hpp"

#include "Core/Audio/IAudioBackend.hpp"

#include <algorithm>
#include <chrono>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

constexpr qint64 TICKS_PER_SECOND = 10'000'000;

std::mutex registryMutex;
std::vector<std::weak_ptr<PlayedSound>> registry;

} // namespace

qint64 PlayedSound::now()
{
    return std::chrono::duration_cast<std::chrono::duration<qint64, std::ratio<1, TICKS_PER_SECOND>>>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PlayedSound::record(const int16_t *stereo, int frames, qint64 handedOverAt)
{
    const std::lock_guard lock(mutex);
    for (int i = 0; i < frames; i++) {
        const size_t slot = static_cast<size_t>((end + i) % KEPT_FRAMES) * 2;
        kept[slot] = stereo[i * 2];
        kept[slot + 1] = stereo[i * 2 + 1];
    }
    end += frames;
    lastHandedOverAt = handedOverAt;
}

qint64 PlayedSound::frameAt(qint64 time) const
{
    const std::lock_guard lock(mutex);
    return end + (time - lastHandedOverAt) * AUDIO_SAMPLE_RATE / TICKS_PER_SECOND;
}

qint64 PlayedSound::recordedEnd() const
{
    const std::lock_guard lock(mutex);
    return end;
}

bool PlayedSound::read(qint64 first, int frames, float *stereo) const
{
    const std::lock_guard lock(mutex);
    if (first < std::max<qint64>(0, end - KEPT_FRAMES) || first + frames > end)
        return false;
    for (int i = 0; i < frames; i++) {
        const size_t slot = static_cast<size_t>((first + i) % KEPT_FRAMES) * 2;
        stereo[i * 2] = kept[slot] / 32768.0f;
        stereo[i * 2 + 1] = kept[slot + 1] / 32768.0f;
    }
    return true;
}

std::shared_ptr<PlayedSound> PlayedSounds::startRecording()
{
    auto sound = std::make_shared<PlayedSound>();
    const std::lock_guard lock(registryMutex);
    std::erase_if(registry, [](const std::weak_ptr<PlayedSound> &each) { return each.expired(); });
    registry.push_back(sound);
    return sound;
}

std::vector<std::shared_ptr<PlayedSound>> PlayedSounds::playing()
{
    std::vector<std::shared_ptr<PlayedSound>> sounds;
    const std::lock_guard lock(registryMutex);
    for (const std::weak_ptr<PlayedSound> &each : registry) {
        if (auto sound = each.lock())
            sounds.push_back(std::move(sound));
    }
    return sounds;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
