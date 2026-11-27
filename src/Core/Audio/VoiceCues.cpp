#include "VoiceCues.hpp"

#include "IAudioBackend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <vector>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

struct Note
{
    double frequency;
    double start; // seconds
};

// two soft bell-like notes: rising for a join, falling for a leave
constexpr double LOW_NOTE = 659.25;  // E5
constexpr double HIGH_NOTE = 880.0;  // A5
constexpr double NOTE_SPACING = 0.085;
constexpr double NOTE_LENGTH = 0.16;
constexpr double ATTACK = 0.006;
constexpr double DECAY = 0.045;      // time constant of the ring-out
constexpr double TAIL_FADE = 0.01;   // so each note ends exactly at zero
constexpr double OVERTONE = 0.25;    // a little second harmonic, so it isn't a bare sine
constexpr double PEAK = 0.22;        // of full scale, per note

QVector<QByteArray> render(std::initializer_list<Note> notes)
{
    double length = 0.0;
    for (const Note &note : notes)
        length = std::max(length, note.start + NOTE_LENGTH);

    const int needed = static_cast<int>(std::ceil(length * AUDIO_SAMPLE_RATE));
    const int frameCount = (needed + AUDIO_FRAME_SAMPLES - 1) / AUDIO_FRAME_SAMPLES;
    std::vector<double> mono(static_cast<size_t>(frameCount) * AUDIO_FRAME_SAMPLES, 0.0);

    const int noteSamples = static_cast<int>(std::lround(NOTE_LENGTH * AUDIO_SAMPLE_RATE));
    for (const Note &note : notes) {
        const auto first = static_cast<size_t>(std::lround(note.start * AUDIO_SAMPLE_RATE));
        for (int i = 0; i < noteSamples && first + i < mono.size(); i++) {
            const double t = static_cast<double>(i) / AUDIO_SAMPLE_RATE;
            const double envelope = std::min(1.0, t / ATTACK) * std::exp(-t / DECAY)
                                    * std::min(1.0, (NOTE_LENGTH - t) / TAIL_FADE);
            const double phase = 2.0 * std::numbers::pi * note.frequency * t;
            const double wave = (std::sin(phase) + OVERTONE * std::sin(2.0 * phase)) / (1.0 + OVERTONE);
            mono[first + i] += PEAK * envelope * wave;
        }
    }

    QVector<QByteArray> frames;
    frames.reserve(frameCount);
    for (int f = 0; f < frameCount; f++) {
        QByteArray frame(AUDIO_FRAME_SIZE, '\0');
        auto *out = reinterpret_cast<int16_t *>(frame.data());
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            const double sample = std::clamp(mono[static_cast<size_t>(f) * AUDIO_FRAME_SAMPLES + i], -1.0, 1.0);
            const auto value = static_cast<int16_t>(std::lround(sample * INT16_MAX));
            for (int c = 0; c < AUDIO_CHANNELS; c++)
                out[i * AUDIO_CHANNELS + c] = value;
        }
        frames.append(frame);
    }
    return frames;
}

} // namespace

const QVector<QByteArray> &voiceCueFrames(VoiceCue cue)
{
    static const QVector<QByteArray> join = render({ { LOW_NOTE, 0.0 }, { HIGH_NOTE, NOTE_SPACING } });
    static const QVector<QByteArray> leave = render({ { HIGH_NOTE, 0.0 }, { LOW_NOTE, NOTE_SPACING } });
    return cue == VoiceCue::Join ? join : leave;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
