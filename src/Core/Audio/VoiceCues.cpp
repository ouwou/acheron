#include "VoiceCues.hpp"

#include "IAudioBackend.hpp"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QtEndian>

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

QVector<QByteArray> builtinVoiceCueFrames(VoiceCue cue)
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

namespace {

QString settingsKey(VoiceCue cue)
{
    switch (cue) {
    case VoiceCue::Join:
        return QStringLiteral("voice/sounds/join");
    case VoiceCue::Leave:
        return QStringLiteral("voice/sounds/leave");
    case VoiceCue::Message:
        break;
    }
    return QStringLiteral("voice/sounds/message");
}

// the chosen sounds, read from their files once; cues play from the pipeline's thread too
struct CustomSound
{
    bool loaded = false;
    QVector<QByteArray> frames; // empty: the built-in chime
};
QMutex customMutex;
QHash<int, CustomSound> customSounds;

double sampleAt(const char *data, int format, int bytes)
{
    if (format == 3) // IEEE float
        return bytes == 8 ? qFromLittleEndian<double>(data) : qFromLittleEndian<float>(data);
    switch (bytes) {
    case 1:
        return (static_cast<unsigned char>(data[0]) - 128) / 128.0;
    case 2:
        return qFromLittleEndian<qint16>(data) / 32768.0;
    case 3: {
        // into the top three bytes of a 32-bit sample, which keeps the sign
        const auto byte = [data](int i) { return static_cast<quint32>(static_cast<unsigned char>(data[i])); };
        return static_cast<qint32>(byte(0) << 8 | byte(1) << 16 | byte(2) << 24) / 2147483648.0;
    }
    default:
        return qFromLittleEndian<qint32>(data) / 2147483648.0;
    }
}

} // namespace

std::optional<QVector<QByteArray>> loadCueSound(const QString &path, QString *error)
{
    auto fail = [error](const QString &why) -> std::optional<QVector<QByteArray>> {
        if (error)
            *error = why;
        return std::nullopt;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > 64 * 1024 * 1024)
        return fail(QStringLiteral("The file is too big for a notification sound."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WAVE")
        return fail(QStringLiteral("Only WAV files can be used."));

    int format = 0, channels = 0, rate = 0, sampleBytes = 0;
    const char *data = nullptr;
    qint64 dataSize = 0;
    for (qint64 pos = 12; pos + 8 <= bytes.size();) {
        const QByteArray id = bytes.mid(pos, 4);
        const qint64 size = qFromLittleEndian<quint32>(bytes.constData() + pos + 4);
        const qint64 body = pos + 8;
        const qint64 available = std::min<qint64>(size, bytes.size() - body);
        if (id == "fmt " && available >= 16) {
            format = qFromLittleEndian<quint16>(bytes.constData() + body);
            channels = qFromLittleEndian<quint16>(bytes.constData() + body + 2);
            rate = static_cast<int>(qFromLittleEndian<quint32>(bytes.constData() + body + 4));
            sampleBytes = qFromLittleEndian<quint16>(bytes.constData() + body + 14) / 8;
            if (format == 0xFFFE && available >= 26) // WAVE_FORMAT_EXTENSIBLE: the real format leads the subformat GUID
                format = qFromLittleEndian<quint16>(bytes.constData() + body + 24);
        } else if (id == "data") {
            data = bytes.constData() + body;
            dataSize = available;
        }
        pos = body + size + (size & 1);
    }

    const bool integer = format == 1 && sampleBytes >= 1 && sampleBytes <= 4;
    const bool floating = format == 3 && (sampleBytes == 4 || sampleBytes == 8);
    if (!data || channels < 1 || rate < 8000 || rate > 384000 || (!integer && !floating))
        return fail(QStringLiteral("This WAV file's format isn't supported (use PCM or float samples)."));

    const qint64 count = dataSize / (static_cast<qint64>(channels) * sampleBytes);
    if (count == 0)
        return fail(QStringLiteral("The file has no audio in it."));
    if (count > static_cast<qint64>(rate) * MAX_CUE_SECONDS)
        return fail(QStringLiteral("Notification sounds can be at most %1 seconds long.").arg(MAX_CUE_SECONDS));

    auto sample = [&](qint64 i, int channel) {
        return sampleAt(data + (i * channels + std::min(channel, channels - 1)) * sampleBytes, format, sampleBytes);
    };

    // to the pipeline's rate, linearly; any extra channels are left out
    const qint64 outCount = (count * AUDIO_SAMPLE_RATE + rate - 1) / rate;
    const int frameCount = static_cast<int>((outCount + AUDIO_FRAME_SAMPLES - 1) / AUDIO_FRAME_SAMPLES);
    QVector<QByteArray> frames;
    frames.reserve(frameCount);
    for (int f = 0; f < frameCount; f++) {
        QByteArray frame(AUDIO_FRAME_SIZE, '\0');
        auto *out = reinterpret_cast<int16_t *>(frame.data());
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            const qint64 j = static_cast<qint64>(f) * AUDIO_FRAME_SAMPLES + i;
            if (j >= outCount)
                break;
            const double position = static_cast<double>(j) * rate / AUDIO_SAMPLE_RATE;
            const auto i0 = std::min(static_cast<qint64>(position), count - 1);
            const qint64 i1 = std::min(i0 + 1, count - 1);
            const double t = position - static_cast<double>(i0);
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                const double v = sample(i0, c) * (1.0 - t) + sample(i1, c) * t;
                out[i * AUDIO_CHANNELS + c] = static_cast<int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * INT16_MAX));
            }
        }
        frames.append(frame);
    }
    return frames;
}

QString voiceCueSoundPath(VoiceCue cue)
{
    return QSettings().value(settingsKey(cue)).toString();
}

bool setVoiceCueSoundPath(VoiceCue cue, const QString &path, QString *error)
{
    QVector<QByteArray> frames;
    if (!path.isEmpty()) {
        auto loaded = loadCueSound(path, error);
        if (!loaded)
            return false;
        frames = std::move(*loaded);
    }

    QSettings settings;
    if (path.isEmpty())
        settings.remove(settingsKey(cue));
    else
        settings.setValue(settingsKey(cue), path);

    const QMutexLocker lock(&customMutex);
    customSounds[static_cast<int>(cue)] = { true, frames };
    return true;
}

QVector<QByteArray> voiceCueFrames(VoiceCue cue)
{
    const QMutexLocker lock(&customMutex);
    CustomSound &custom = customSounds[static_cast<int>(cue)];
    if (!custom.loaded) {
        custom.loaded = true;
        const QString path = QSettings().value(settingsKey(cue)).toString();
        if (!path.isEmpty()) {
            QString error;
            if (auto frames = loadCueSound(path, &error))
                custom.frames = std::move(*frames);
            else
                qWarning() << "Can't use the sound" << path << "so the built-in one plays instead:" << error;
        }
    }
    return custom.frames.isEmpty() ? builtinVoiceCueFrames(cue) : custom.frames;
}

DeviceCuePlayer::DeviceCuePlayer(QObject *parent) : QObject(parent)
{
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(AUDIO_FRAME_DURATION_MS);
    connect(&timer, &QTimer::timeout, this, &DeviceCuePlayer::tick);
}

DeviceCuePlayer::~DeviceCuePlayer()
{
    stop();
}

// a few frames ahead so the device doesn't start on an underrun; the playback buffer holds 8
static constexpr int PRIMED_FRAMES = 4;

bool DeviceCuePlayer::play(IAudioBackend *output, const QVector<QByteArray> &cueFrames)
{
    stop();
    if (!output || cueFrames.isEmpty() || !output->startPlayback())
        return false;

    backend = output;
    frames = cueFrames;
    for (next = 0; next < std::min(PRIMED_FRAMES, static_cast<int>(frames.size())); next++)
        backend->pushPlaybackFrame(reinterpret_cast<const int16_t *>(frames[next].constData()));
    timer.start();
    return true;
}

void DeviceCuePlayer::tick()
{
    if (next < frames.size())
        backend->pushPlaybackFrame(reinterpret_cast<const int16_t *>(frames[next].constData()));
    // once the primed frames have drained too
    else if (next >= frames.size() + PRIMED_FRAMES + 1)
        stop();
    next++;
}

void DeviceCuePlayer::stop(bool closeDevice)
{
    timer.stop();
    if (backend && closeDevice)
        backend->stopPlayback();
    backend = nullptr;
    frames.clear();
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
