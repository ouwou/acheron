#pragma once

#include <QtGlobal>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Acheron {
namespace Core {
namespace Audio {

class PlayedSound
{
public:
    static constexpr int KEPT_FRAMES = 1 << 17;
    static constexpr qint64 TICKS_PER_SECOND = 10'000'000;

    // the steady clock, which on Windows is the performance counter WASAPI stamps captures with
    [[nodiscard]] static qint64 now();

    // the frames that follow the last ones recorded, handed to the device at `handedOverAt`
    void record(const int16_t *stereo, int frames, qint64 handedOverAt);

    // index of the frame handed to the device at `time`, going by the latest recording
    [[nodiscard]] qint64 frameAt(qint64 time) const;
    [[nodiscard]] qint64 recordedEnd() const;
    [[nodiscard]] bool read(qint64 first, int frames, float *stereo) const;

private:
    mutable std::mutex mutex;
    std::vector<int16_t> kept = std::vector<int16_t>(static_cast<size_t>(KEPT_FRAMES) * 2);
    qint64 end = 0;
    qint64 lastHandedOverAt = 0;
};

class PlayedSounds
{
public:
    [[nodiscard]] static std::shared_ptr<PlayedSound> startRecording();
    [[nodiscard]] static std::vector<std::shared_ptr<PlayedSound>> playing();
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
