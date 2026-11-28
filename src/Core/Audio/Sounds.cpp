#include "Sounds.hpp"

#include "Core/Audio/Miniaudio.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <iterator>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

constexpr qint64 MAX_SOUND_FILE_BYTES = static_cast<qint64>(MAX_SOUND_FILE_MB) * 1024 * 1024;
constexpr int MAX_SOUND_SECONDS = 10;
constexpr const char *SOUND_SUFFIXES[] = { "wav", "mp3", "flac" };

bool isSoundSuffix(const QString &suffix)
{
    const QString lower = suffix.toLower();
    return std::any_of(std::begin(SOUND_SUFFIXES), std::end(SOUND_SUFFIXES), [&lower](const char *known) { return lower == QLatin1String(known); });
}

QFileInfoList customSoundFiles(Sound sound)
{
    const QString name = soundName(sound);
    QFileInfoList found;
    const QFileInfoList candidates = QDir(customSoundsFolder()).entryInfoList({ name + QStringLiteral(".*") }, QDir::Files, QDir::Name);
    for (const QFileInfo &file : candidates) {
        if (file.completeBaseName() == name && isSoundSuffix(file.suffix()))
            found.append(file);
    }
    return found;
}

struct CachedSound
{
    QString path;
    QDateTime modified;
    qint64 size = -1;
    std::shared_ptr<const SoundClip> clip;
};

} // namespace

QString soundName(Sound sound)
{
    switch (sound) {
    case Sound::UserJoin:
        return QStringLiteral("user_join");
    case Sound::UserLeave:
        return QStringLiteral("user_leave");
    case Sound::Disconnect:
        return QStringLiteral("disconnect");
    }
    return {};
}

QString customSoundsFolder()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("sounds"));
}

QString customSoundFile(Sound sound)
{
    const QFileInfoList files = customSoundFiles(sound);
    return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

std::optional<SoundFileProblem> setCustomSound(Sound sound, const QString &path)
{
    const QFileInfo source(path);
    const QString suffix = source.suffix().toLower();
    if (!isSoundSuffix(suffix))
        return SoundFileProblem::NotWavMp3OrFlac;
    if (source.size() > MAX_SOUND_FILE_BYTES)
        return SoundFileProblem::TooBig;

    QFile file(path);
    const QByteArray bytes = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    if (!decodeSound(bytes))
        return SoundFileProblem::NotASound;

    QSaveFile saved(QDir(customSoundsFolder()).filePath(soundName(sound) + QLatin1Char('.') + suffix));
    if (!QDir().mkpath(customSoundsFolder()) || !saved.open(QIODevice::WriteOnly) || saved.write(bytes) != bytes.size() || !saved.commit())
        return SoundFileProblem::NotSaved;

    for (const QFileInfo &old : customSoundFiles(sound)) {
        if (old.suffix().toLower() != suffix)
            QFile::remove(old.absoluteFilePath());
    }
    return std::nullopt;
}

void resetCustomSound(Sound sound)
{
    for (const QFileInfo &file : customSoundFiles(sound))
        QFile::remove(file.absoluteFilePath());
}

std::shared_ptr<const SoundClip> loadSound(Sound sound)
{
    static std::array<CachedSound, std::size(ALL_SOUNDS)> cache;
    CachedSound &cached = cache[static_cast<size_t>(sound)];

    QString path = customSoundFile(sound);
    if (path.isEmpty())
        path = QStringLiteral(":/sounds/%1.wav").arg(soundName(sound));
    const QFileInfo info(path);
    if (path == cached.path && info.lastModified() == cached.modified && info.size() == cached.size)
        return cached.clip;

    cached = { path, info.lastModified(), info.size(), nullptr };
    QFile file(path);
    if (info.size() <= MAX_SOUND_FILE_BYTES && file.open(QIODevice::ReadOnly))
        cached.clip = decodeSound(file.readAll());
    return cached.clip;
}

std::shared_ptr<const SoundClip> decodeSound(const QByteArray &file)
{
    const ma_decoder_config config = ma_decoder_config_init(ma_format_s16, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);
    ma_decoder decoder;
    if (file.isEmpty() || ma_decoder_init_memory(file.constData(), static_cast<size_t>(file.size()), &config, &decoder) != MA_SUCCESS)
        return nullptr;

    constexpr int FRAME_BYTES = AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES;
    constexpr ma_uint64 MAX_FRAMES = static_cast<ma_uint64>(AUDIO_SAMPLE_RATE) * MAX_SOUND_SECONDS;
    QByteArray pcm(static_cast<int>(MAX_FRAMES) * FRAME_BYTES, Qt::Uninitialized);
    ma_uint64 total = 0;
    while (total < MAX_FRAMES) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, pcm.data() + total * FRAME_BYTES, MAX_FRAMES - total, &read);
        total += read;
        if (result != MA_SUCCESS || read == 0)
            break;
    }
    ma_decoder_uninit(&decoder);

    if (total == 0)
        return nullptr;
    pcm.truncate(static_cast<int>(total) * FRAME_BYTES);
    return std::make_shared<const SoundClip>(SoundClip{ std::move(pcm) });
}

void playSoundOn(IAudioBackend &audio, std::shared_ptr<const SoundClip> clip, const QByteArray &outputDevice, float outputVolume)
{
    if (!audio.isPlaying()) {
        audio.setOutputDevice(outputDevice);
        audio.setOutputVolume(outputVolume);
    }
    audio.playSound(std::move(clip), OFFICIAL_SOUND_GAIN);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
