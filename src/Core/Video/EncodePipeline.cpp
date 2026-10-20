#include "Core/Video/EncodePipeline.hpp"

#include "Core/Video/EncodeThread.hpp"

#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

constexpr int MIN_BITRATE = 50'000;
constexpr int BITRATE_WHEN_NONE_GIVEN = 2'500'000;

} // namespace

EncodePipeline::EncodePipeline(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<EncodedFramePointer>("Acheron::Core::Video::EncodedFramePointer");
    qRegisterMetaType<PipelineStats>("Acheron::Core::Video::PipelineStats");
}

EncodePipeline::~EncodePipeline() = default;

int usableBitrate(int wanted, int most)
{
    if (wanted <= 0)
        wanted = BITRATE_WHEN_NONE_GIVEN;
    if (most > 0)
        wanted = std::min(wanted, most);
    return std::max(wanted, MIN_BITRATE);
}

QImage PreviewMaker::make(const AVFrame *frame)
{
    const AVFrame *picture = frame;
    if (frame->hw_frames_ctx) {
        av_frame_unref(downloaded.get());
        if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0)
            return {};
        av_frame_copy_props(downloaded.get(), frame);
        picture = downloaded.get();
    }

    const QSize full(picture->width, picture->height);
    const bool fits = full.width() <= bound.width() && full.height() <= bound.height();
    const QSize size = fits ? full : full.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    if (image.size() != size)
        image = Media::RgbScaler::allocate(size, QImage::Format_RGB32);
    const bool scaled = scaler.scale(picture, image);
    av_frame_unref(downloaded.get());
    return scaled ? image : QImage();
}

int64_t RtpClock::timestampOf(std::chrono::steady_clock::time_point capturedAt)
{
    const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(capturedAt - origin).count();
    last = std::max(last + 1, microseconds * 9 / 100);
    return last;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
