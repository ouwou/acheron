#pragma once

#include "Core/Audio/PlayedSound.hpp"

#include <QtGlobal>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct AVTXContext;

namespace Acheron {
namespace Core {
namespace Audio {

// the subtraction is exact only while the device mixes at 48 kHz without effects
class PlayedSoundRemover
{
public:
    using Sources = std::function<std::vector<std::shared_ptr<PlayedSound>>()>;

    explicit PlayedSoundRemover(Sources sources = PlayedSounds::playing);
    ~PlayedSoundRemover();

    PlayedSoundRemover(const PlayedSoundRemover &) = delete;
    PlayedSoundRemover &operator=(const PlayedSoundRemover &) = delete;

    // `stereo` is 48 kHz interleaved, its first frame captured at `capturedAt` (PlayedSound::now's clock)
    void remove(float *stereo, int frames, qint64 capturedAt);
    void forgetDelays();

    [[nodiscard]] std::optional<qint64> framesLateOf(const std::shared_ptr<PlayedSound> &sound) const;

private:
    struct Tracked
    {
        std::weak_ptr<PlayedSound> sound;
        // the capture's frame n holds the stream's frame n + offset
        std::optional<qint64> offset;
        std::optional<qint64> candidate;
        float gain = 1.0f;
    };

    void follow();
    void align(Tracked &stream);
    [[nodiscard]] float capturedAt(qint64 frame, int channel) const;

    Sources sources;
    std::vector<Tracked> tracked;
    std::vector<float> captured;
    std::vector<float> playedBlock;
    qint64 capturedEnd = 0;
    qint64 capturedEndTime = 0;
    qint64 framesSinceAlignment = 0;

    using Transform = void (*)(AVTXContext *, void *, void *, ptrdiff_t);
    AVTXContext *forward = nullptr;
    AVTXContext *inverse = nullptr;
    Transform forwardTransform = nullptr;
    Transform inverseTransform = nullptr;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
