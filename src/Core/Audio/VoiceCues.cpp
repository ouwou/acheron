#include "VoiceCues.hpp"

#include "IAudioBackend.hpp"

#include <algorithm>
#include <array>
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

struct Harmonic
{
    double multiple;
    double amplitude;
};

// how each note of a chime sounds
struct Voice
{
    double length;   // seconds each note lasts
    double decay;    // time constant of the ring-out
    double peak;     // of full scale, per note
    std::array<Harmonic, 3> harmonics; // unused ones have amplitude 0
};

constexpr double ATTACK = 0.006;
constexpr double TAIL_FADE = 0.01; // so each note ends exactly at zero

// joins and leaves: two soft, ringing bell notes, rising or falling
constexpr Voice BELL { 0.16, 0.045, 0.22, { { { 1.0, 1.0 }, { 2.0, 0.25 }, { 0.0, 0.0 } } } };
constexpr double LOW_NOTE = 659.25; // E5
constexpr double HIGH_NOTE = 880.0; // A5
constexpr double NOTE_SPACING = 0.085;

// messages: three quick, bright plucks going up, so they can't be mistaken for someone joining
constexpr Voice PLUCK { 0.075, 0.018, 0.34, { { { 1.0, 1.0 }, { 3.0, 0.33 }, { 5.0, 0.12 } } } };
constexpr double MESSAGE_NOTES[] = { 1046.5, 1318.5, 1568.0 }; // C6, E6, G6
constexpr double MESSAGE_SPACING = 0.055;

QVector<QByteArray> render(const Voice &voice, std::initializer_list<Note> notes)
{
    double length = 0.0;
    for (const Note &note : notes)
        length = std::max(length, note.start + voice.length);

    const int needed = static_cast<int>(std::ceil(length * AUDIO_SAMPLE_RATE));
    const int frameCount = (needed + AUDIO_FRAME_SAMPLES - 1) / AUDIO_FRAME_SAMPLES;
    std::vector<double> mono(static_cast<size_t>(frameCount) * AUDIO_FRAME_SAMPLES, 0.0);

    double harmonicSum = 0.0;
    for (const Harmonic &h : voice.harmonics)
        harmonicSum += h.amplitude;

    const int noteSamples = static_cast<int>(std::lround(voice.length * AUDIO_SAMPLE_RATE));
    for (const Note &note : notes) {
        const auto first = static_cast<size_t>(std::lround(note.start * AUDIO_SAMPLE_RATE));
        for (int i = 0; i < noteSamples && first + i < mono.size(); i++) {
            const double t = static_cast<double>(i) / AUDIO_SAMPLE_RATE;
            const double envelope = std::min(1.0, t / ATTACK) * std::exp(-t / voice.decay)
                                    * std::min(1.0, (voice.length - t) / TAIL_FADE);
            const double phase = 2.0 * std::numbers::pi * note.frequency * t;
            double wave = 0.0;
            for (const Harmonic &h : voice.harmonics)
                wave += h.amplitude * std::sin(h.multiple * phase);
            mono[first + i] += voice.peak * envelope * wave / harmonicSum;
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
    static const QVector<QByteArray> join = render(BELL, { { LOW_NOTE, 0.0 }, { HIGH_NOTE, NOTE_SPACING } });
    static const QVector<QByteArray> leave = render(BELL, { { HIGH_NOTE, 0.0 }, { LOW_NOTE, NOTE_SPACING } });
    static const QVector<QByteArray> message = render(PLUCK, { { MESSAGE_NOTES[0], 0.0 },
                                                               { MESSAGE_NOTES[1], MESSAGE_SPACING },
                                                               { MESSAGE_NOTES[2], 2 * MESSAGE_SPACING } });
    switch (cue) {
    case VoiceCue::Join:
        return join;
    case VoiceCue::Leave:
        return leave;
    case VoiceCue::Message:
        break;
    }
    return message;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
