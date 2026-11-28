#pragma once

#include "Core/Audio/IAudioBackend.hpp"

#include <QString>

#include <memory>
#include <optional>

namespace Acheron {
namespace Core {
namespace Audio {

enum class Sound {
    UserJoin,
    UserLeave,
    Disconnect,
};

inline constexpr Sound ALL_SOUNDS[] = { Sound::UserJoin, Sound::UserLeave, Sound::Disconnect };

inline constexpr float OFFICIAL_SOUND_GAIN = 0.4f;
inline constexpr int MAX_OTHERS_FOR_JOIN_LEAVE_SOUNDS = 25; // the official client's limit
inline constexpr int MAX_SOUND_FILE_MB = 10;

[[nodiscard]] constexpr std::optional<Sound> joinLeaveSound(int othersBefore, int othersAfter)
{
    if (othersBefore > MAX_OTHERS_FOR_JOIN_LEAVE_SOUNDS || othersAfter == othersBefore)
        return std::nullopt;
    return othersAfter > othersBefore ? Sound::UserJoin : Sound::UserLeave;
}

// the official client's names, so its files drop straight in
[[nodiscard]] QString soundName(Sound sound);

enum class SoundFileProblem {
    NotWavMp3OrFlac,
    TooBig,
    NotASound,
    NotSaved,
};

[[nodiscard]] QString customSoundsFolder();
[[nodiscard]] QString customSoundFile(Sound sound);
// copied into the folder, so the original can go
[[nodiscard]] std::optional<SoundFileProblem> setCustomSound(Sound sound, const QString &path);
void resetCustomSound(Sound sound);

// gui thread; cached until the file changes
[[nodiscard]] std::shared_ptr<const SoundClip> loadSound(Sound sound);
[[nodiscard]] std::shared_ptr<const SoundClip> decodeSound(const QByteArray &file);

// on the chosen output, unless a call already has it open
void playSoundOn(IAudioBackend &audio, std::shared_ptr<const SoundClip> clip, const QByteArray &outputDevice, float outputVolume);

} // namespace Audio
} // namespace Core
} // namespace Acheron
