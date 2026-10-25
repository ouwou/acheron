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

// takes what this program played back out of a capture of the whole output device, which is all Windows
// before 10 can capture. each playback stream is found in the capture by cross-correlation, then
// subtracted at that delay with a fitted gain: exact while the device mixes at 48 kHz without effects
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
    // the capture skipped, so every delay found is wrong
    void startOver();

    // how many frames later a stream's sound shows up in the capture, once found
    [[nodiscard]] std::optional<qint64> delayOf(const std::shared_ptr<PlayedSound> &sound) const;

    static constexpr int WINDOW_FRAMES = 9600;
    static constexpr qint64 LONGEST_DELAY_MS = 500;

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
    void align(Tracked &tracked);
    [[nodiscard]] float capturedAt(qint64 frame, int channel) const;

    Sources sources;
    std::vector<Tracked> tracked;
    std::vector<float> captured;
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
