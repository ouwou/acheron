#pragma once

#include <QCoreApplication>
#include <QStringList>

#include "Core/Media/VideoDecoder.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Streams/StreamSession.hpp"
#include "Core/Video/CameraSender.hpp"

namespace Acheron {
namespace UI {

class VideoSurface;

// The lines a tile shows over itself while the call view's stats are on.
class CallStats
{
    Q_DECLARE_TR_FUNCTIONS(CallStats)
public:
    [[nodiscard]] static QStringList ofOwnShare(const Core::StreamBroadcaster::Stats &stats);
    [[nodiscard]] static QStringList ofOwnCamera(const Core::CameraSender::Stats &stats);
    [[nodiscard]] static QStringList ofStream(const Core::StreamSession::Stats &stats, const VideoSurface *surface);
    // Empty until the camera's first frame has been decoded.
    [[nodiscard]] static QStringList ofCamera(const Core::Media::VideoDecoder::Stats &stats, const VideoSurface *surface);
};

} // namespace UI
} // namespace Acheron
