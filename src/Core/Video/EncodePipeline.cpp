#include "Core/Video/EncodePipeline.hpp"

#include "Core/Video/CaptureCommon.hpp"
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

int usableBitrate(std::optional<int> wanted, std::optional<int> most)
{
    const int bitrate = wanted.value_or(BITRATE_WHEN_NONE_GIVEN);
    return std::max(most ? std::min(bitrate, *most) : bitrate, MIN_BITRATE);
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

    const QSize size = shrunkToFit(QSize(picture->width, picture->height), bound);
    if (image.size() != size)
        image = Media::RgbScaler::allocate(size, QImage::Format_RGB32);
    const bool scaled = scaler.scale(picture, image);
    av_frame_unref(downloaded.get());
    return scaled ? image : QImage();
}

void EncodeCounters::deliver(std::vector<Discord::Voice::EncodedVideoFrame> &encoded, EncodePipeline *pipeline, int layer)
{
    framesDropped += encoded.empty() ? 1 : 0;
    for (Discord::Voice::EncodedVideoFrame &each : encoded) {
        framesEncoded++;
        encodedLately++;
        bytesLately += each.data.size();
        emit pipeline->frameEncoded(std::make_shared<const Discord::Voice::EncodedVideoFrame>(std::move(each)), layer);
    }
}

PipelineStats EncodeCounters::takeStats(double seconds, const Media::VideoEncoder::Stats &encoder, const QString &encoderName, const QString &source, const QSize &encodedSize, int capturedLately)
{
    PipelineStats stats;
    stats.encoderName = QStringLiteral("%1, from %2").arg(encoderName, source);
    stats.hardware = encoder.hardware;
    stats.encodedSize = encodedSize;
    stats.capturedFps = capturedLately / seconds;
    stats.encodedFps = encodedLately / seconds;
    stats.averageEncodeMs = encoder.averageEncodeMs;
    stats.bitrateBps = static_cast<quint64>(static_cast<double>(bytesLately) * 8.0 / seconds);
    stats.framesEncoded = framesEncoded;
    stats.framesDropped = framesDropped;
    encodedLately = 0;
    bytesLately = 0;
    return stats;
}

QString encoderOrNone(const Media::VideoEncoder::Stats &encoder)
{
    return encoder.encoderName.isEmpty() ? QStringLiteral("no encoder yet") : encoder.encoderName;
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
