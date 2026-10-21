#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <memory>
#include <numeric>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {
class VoiceClient;
}
} // namespace Discord

namespace Core {

namespace Audio {
class OpusEncoder;
}

// captured silence is 16-bit rounding noise, measured at an RMS of 0.5 for a program playing nothing
// and up to 1.8 for everything but one program
[[nodiscard]] inline bool isSilence(const QByteArray &pcm)
{
    constexpr qint64 LOUDEST_SILENCE_RMS = 6;

    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const qint64 count = pcm.size() / static_cast<qint64>(sizeof(qint16));
    const qint64 squares = std::accumulate(samples, samples + count, qint64(0), [](qint64 sum, qint16 sample) { return sum + qint64(sample) * sample; });
    return squares <= LOUDEST_SILENCE_RMS * LOUDEST_SILENCE_RMS * count;
}

class SoundRuns
{
public:
    enum class Step {
        Skip,
        Begin,
        Continue,
        End,
    };

    static constexpr int HELD_SILENT_FRAMES = 25;

    Step offer(bool silent)
    {
        if (!silent) {
            silentFramesLeft = HELD_SILENT_FRAMES;
            return std::exchange(open, true) ? Step::Continue : Step::Begin;
        }
        if (!open)
            return Step::Skip;
        if (silentFramesLeft > 0) {
            silentFramesLeft--;
            return Step::Continue;
        }
        open = false;
        return Step::End;
    }

    bool stop() { return std::exchange(open, false); }

private:
    bool open = false;
    int silentFramesLeft = 0;
};

class StreamAudio : public QObject
{
    Q_OBJECT
public:
    // what the official client raises stream audio to, unless the voice bitrate is higher
    static constexpr int MIN_BITRATE = 128000;
    // nothing is captured while the machine plays nothing
    static constexpr int SOUND_GONE_MS = 100;

    StreamAudio(Discord::Voice::VoiceClient *rtc, int bitrate);
    ~StreamAudio() override;

public slots:
    void onFrameCaptured(const QByteArray &pcm);

private slots:
    void onSoundGone();

private:
    void sendTrailingSilence();

    Discord::Voice::VoiceClient *rtc;
    std::unique_ptr<Audio::OpusEncoder> encoder;
    SoundRuns runs;
    QTimer soundGone;

    static constexpr int TRAILING_SILENCE_FRAMES = 5;
};

} // namespace Core
} // namespace Acheron
