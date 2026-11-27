#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <optional>

namespace Acheron {
namespace Core {
namespace Audio {

class IAudioBackend;

enum class VoiceCue {
    Join,
    Leave,
    // not a voice event: a new direct message
    Message,
};

// what a cue plays: the sound file chosen for it under Settings > Voice > Sounds, otherwise the built-in
// chime. Whole frames in the pipeline's playback format (AUDIO_FRAME_SIZE bytes of 48 kHz stereo s16 each).
[[nodiscard]] QVector<QByteArray> voiceCueFrames(VoiceCue cue);
// the generated chime whatever is chosen; generated once, so there are no sound files to ship
[[nodiscard]] QVector<QByteArray> builtinVoiceCueFrames(VoiceCue cue);

// the WAV file chosen for a cue, empty for the built-in chime
[[nodiscard]] QString voiceCueSoundPath(VoiceCue cue);
// an empty path goes back to the built-in chime; false, with error saying why, if the file can't be used
bool setVoiceCueSoundPath(VoiceCue cue, const QString &path, QString *error = nullptr);

constexpr int MAX_CUE_SECONDS = 5;
// a WAV file (integer PCM or float, any sample rate, any channels; the first two are used) as frames
[[nodiscard]] std::optional<QVector<QByteArray>> loadCueSound(const QString &path, QString *error);

// feeds a cue to an output device a frame per tick, for when there's no call to mix it into
class DeviceCuePlayer : public QObject
{
public:
    explicit DeviceCuePlayer(QObject *parent = nullptr);
    ~DeviceCuePlayer() override;

    // false if the device couldn't be opened
    bool play(IAudioBackend *output, const QVector<QByteArray> &frames);
    // closeDevice false when a call is about to take the device over itself
    void stop(bool closeDevice = true);
    [[nodiscard]] bool isPlaying() const { return backend != nullptr; }

private:
    void tick();

    QTimer timer;
    IAudioBackend *backend = nullptr;
    QVector<QByteArray> frames;
    int next = 0;
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
