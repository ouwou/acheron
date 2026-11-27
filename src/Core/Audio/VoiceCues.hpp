#pragma once

#include <QByteArray>
#include <QVector>

namespace Acheron {
namespace Core {
namespace Audio {

enum class VoiceCue {
    Join,
    Leave,
};

// a short chime for someone joining or leaving the call, as whole frames in the pipeline's playback format
// (AUDIO_FRAME_SIZE bytes of 48 kHz stereo s16 each); generated once, so there are no sound files to ship
[[nodiscard]] const QVector<QByteArray> &voiceCueFrames(VoiceCue cue);

} // namespace Audio
} // namespace Core
} // namespace Acheron
