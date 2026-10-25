#include "Core/Audio/PlayedSoundRemover.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/tx.h>
}

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

constexpr qint64 TICKS_PER_MS = 10'000;
constexpr qint64 LATE_MARGIN_MS = 50;
constexpr int SEARCHED_DELAYS = static_cast<int>((PlayedSoundRemover::LONGEST_DELAY_MS + LATE_MARGIN_MS) * AUDIO_SAMPLE_RATE / 1000);
constexpr int TRANSFORM_SIZE = 1 << 16;
static_assert(PlayedSoundRemover::WINDOW_FRAMES + SEARCHED_DELAYS <= TRANSFORM_SIZE, "the correlation would wrap around");
constexpr int CAPTURE_KEPT_FRAMES = 1 << 15;
static_assert(PlayedSoundRemover::WINDOW_FRAMES <= CAPTURE_KEPT_FRAMES);

constexpr qint64 SEARCH_EVERY_FRAMES = AUDIO_SAMPLE_RATE / 4;
constexpr qint64 CHECK_EVERY_FRAMES = AUDIO_SAMPLE_RATE;
// chance peaks over every delay searched stay under 0.05 for a window this long, and a delay counts only
// once two searches in a row find it
constexpr float LEAST_CORRELATION = 0.08f;
constexpr float QUIET_MEAN_SQUARE = 1e-7f;
// devices that level their sound change its gain from one moment to the next, so it is fitted again for
// every block, moving this far towards the block's own fit
constexpr double GAIN_ADAPTATION = 0.5;
constexpr double LARGEST_GAIN = 2.0;

bool near(qint64 a, qint64 b)
{
    return std::abs(a - b) <= 1;
}

} // namespace

PlayedSoundRemover::PlayedSoundRemover(Sources sources) : sources(std::move(sources)), captured(static_cast<size_t>(CAPTURE_KEPT_FRAMES) * 2)
{
    const float unscaled = 1.0f;
    if (av_tx_init(&forward, &forwardTransform, AV_TX_FLOAT_FFT, 0, TRANSFORM_SIZE, &unscaled, 0) < 0 || av_tx_init(&inverse, &inverseTransform, AV_TX_FLOAT_FFT, 1, TRANSFORM_SIZE, &unscaled, 0) < 0)
        qCWarning(LogVoice) << "No transform for finding this program's own sound; the shared sound keeps it";
}

PlayedSoundRemover::~PlayedSoundRemover()
{
    av_tx_uninit(&forward);
    av_tx_uninit(&inverse);
}

void PlayedSoundRemover::remove(float *stereo, int frames, qint64 capturedAt)
{
    const qint64 first = capturedEnd;
    for (int i = 0; i < frames; i++) {
        const size_t slot = static_cast<size_t>((first + i) % CAPTURE_KEPT_FRAMES) * 2;
        captured[slot] = stereo[i * 2];
        captured[slot + 1] = stereo[i * 2 + 1];
    }
    capturedEnd += frames;
    capturedEndTime = capturedAt + frames * 1000 * TICKS_PER_MS / AUDIO_SAMPLE_RATE;

    follow();
    std::vector<float> played(static_cast<size_t>(frames) * 2);
    for (Tracked &each : tracked) {
        const auto sound = each.sound.lock();
        if (!sound || !each.offset || !sound->read(first + *each.offset, frames, played.data()))
            continue;
        double together = 0.0;
        double alone = 0.0;
        for (int i = 0; i < frames * 2; i++) {
            together += static_cast<double>(stereo[i]) * played[i];
            alone += static_cast<double>(played[i]) * played[i];
        }
        if (alone > QUIET_MEAN_SQUARE * frames * 2)
            each.gain = static_cast<float>(std::clamp(each.gain + GAIN_ADAPTATION * (together / alone - each.gain), 0.0, LARGEST_GAIN));
        for (int i = 0; i < frames * 2; i++)
            stereo[i] -= each.gain * played[i];
    }

    framesSinceAlignment += frames;
    const bool settled = std::all_of(tracked.begin(), tracked.end(), [](const Tracked &each) { return each.offset && !each.candidate; });
    if (framesSinceAlignment < (settled ? CHECK_EVERY_FRAMES : SEARCH_EVERY_FRAMES))
        return;
    framesSinceAlignment = 0;
    for (Tracked &each : tracked)
        align(each);
}

void PlayedSoundRemover::startOver()
{
    for (Tracked &each : tracked) {
        each.offset.reset();
        each.candidate.reset();
    }
}

std::optional<qint64> PlayedSoundRemover::delayOf(const std::shared_ptr<PlayedSound> &sound) const
{
    for (const Tracked &each : tracked) {
        if (each.sound.lock() == sound && each.offset)
            return -*each.offset;
    }
    return std::nullopt;
}

void PlayedSoundRemover::follow()
{
    std::erase_if(tracked, [](const Tracked &each) { return each.sound.expired(); });
    for (const std::shared_ptr<PlayedSound> &sound : sources()) {
        const bool known = std::any_of(tracked.begin(), tracked.end(), [&sound](const Tracked &each) { return each.sound.lock() == sound; });
        if (!known)
            tracked.push_back({ sound });
    }
}

float PlayedSoundRemover::capturedAt(qint64 frame, int channel) const
{
    return captured[static_cast<size_t>(frame % CAPTURE_KEPT_FRAMES) * 2 + channel];
}

void PlayedSoundRemover::align(Tracked &tracked)
{
    const auto sound = tracked.sound.lock();
    if (!sound || !forward || !inverse || capturedEnd < WINDOW_FRAMES)
        return;

    const qint64 windowStart = capturedEnd - WINDOW_FRAMES;
    const qint64 recordedEnd = sound->recordedEnd();
    const qint64 lowest = std::max(sound->frameAt(capturedEndTime - LONGEST_DELAY_MS * TICKS_PER_MS), recordedEnd - PlayedSound::KEPT_FRAMES + WINDOW_FRAMES) - capturedEnd;
    const qint64 highest = std::min({ sound->frameAt(capturedEndTime + LATE_MARGIN_MS * TICKS_PER_MS), recordedEnd, sound->frameAt(capturedEndTime - LONGEST_DELAY_MS * TICKS_PER_MS) + SEARCHED_DELAYS }) - capturedEnd;
    if (highest <= lowest)
        return;
    const int delays = static_cast<int>(highest - lowest);
    const int playedFrames = WINDOW_FRAMES + delays;
    std::vector<float> played(static_cast<size_t>(playedFrames) * 2);
    if (!sound->read(windowStart + lowest, playedFrames, played.data()))
        return;

    std::vector<AVComplexFloat> capturedIn(TRANSFORM_SIZE, { 0.0f, 0.0f });
    std::vector<AVComplexFloat> playedIn(TRANSFORM_SIZE, { 0.0f, 0.0f });
    std::vector<float> playedEnergy(static_cast<size_t>(playedFrames) + 1, 0.0f);
    double capturedEnergy = 0.0;
    for (int i = 0; i < WINDOW_FRAMES; i++) {
        capturedIn[i].re = capturedAt(windowStart + i, 0) + capturedAt(windowStart + i, 1);
        capturedEnergy += static_cast<double>(capturedIn[i].re) * capturedIn[i].re;
    }
    for (int i = 0; i < playedFrames; i++) {
        playedIn[i].re = played[i * 2] + played[i * 2 + 1];
        playedEnergy[i + 1] = playedEnergy[i] + playedIn[i].re * playedIn[i].re;
    }
    if (playedEnergy.back() / playedFrames < QUIET_MEAN_SQUARE || capturedEnergy / WINDOW_FRAMES < QUIET_MEAN_SQUARE)
        return;

    std::vector<AVComplexFloat> capturedSpectrum(TRANSFORM_SIZE);
    std::vector<AVComplexFloat> playedSpectrum(TRANSFORM_SIZE);
    forwardTransform(forward, capturedSpectrum.data(), capturedIn.data(), sizeof(AVComplexFloat));
    forwardTransform(forward, playedSpectrum.data(), playedIn.data(), sizeof(AVComplexFloat));
    for (int k = 0; k < TRANSFORM_SIZE; k++) {
        const AVComplexFloat c = capturedSpectrum[k];
        const AVComplexFloat p = playedSpectrum[k];
        playedIn[k] = { c.re * p.re + c.im * p.im, c.re * p.im - c.im * p.re };
    }
    inverseTransform(inverse, capturedIn.data(), playedIn.data(), sizeof(AVComplexFloat));

    int best = -1;
    double bestCorrelation = 0.0;
    for (int k = 0; k <= delays; k++) {
        const double energy = static_cast<double>(playedEnergy[k + WINDOW_FRAMES]) - playedEnergy[k];
        if (energy <= 0.0)
            continue;
        // ffmpeg leaves a transform there and back unscaled
        const double correlation = capturedIn[k].re / TRANSFORM_SIZE / std::sqrt(capturedEnergy * energy);
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            best = k;
        }
    }
    if (best < 0 || bestCorrelation < LEAST_CORRELATION) {
        tracked.candidate.reset();
        return;
    }

    double together = 0.0;
    double alone = 0.0;
    for (int i = 0; i < WINDOW_FRAMES; i++) {
        for (int channel = 0; channel < 2; channel++) {
            const double p = played[static_cast<size_t>(i + best) * 2 + channel];
            together += capturedAt(windowStart + i, channel) * p;
            alone += p * p;
        }
    }
    const auto gain = static_cast<float>(std::clamp(together / alone, 0.0, LARGEST_GAIN));
    const qint64 found = lowest + best;

    if (tracked.offset && near(found, *tracked.offset)) {
        tracked.candidate.reset();
        return;
    }
    if (tracked.candidate && near(found, *tracked.candidate)) {
        tracked.offset = found;
        tracked.gain = gain;
        tracked.candidate.reset();
        qCInfo(LogVoice) << "This program's own sound is" << -found << "frames late in the device capture, at a gain of" << gain << "( correlation" << bestCorrelation << ")";
        return;
    }
    tracked.candidate = found;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
