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

class CallStats
{
    Q_DECLARE_TR_FUNCTIONS(CallStats)
public:
    [[nodiscard]] static QStringList ofOwnShare(const Core::StreamBroadcaster::Stats &stats);
    [[nodiscard]] static QStringList ofOwnCamera(const Core::CameraSender::Stats &stats);
    [[nodiscard]] static QStringList ofStream(const Core::StreamSession::Stats &stats, const VideoSurface *surface);
    [[nodiscard]] static QStringList ofCamera(const Core::Media::VideoDecoder::Stats &stats, const VideoSurface *surface);

private:
    [[nodiscard]] static QStringList decoderLines(const Core::Media::VideoDecoder::Stats &stats, const VideoSurface *surface);
};

} // namespace UI
} // namespace Acheron
