#include "Core/Media/RgbScaler.hpp"

#ifdef ACHERON_HAVE_FFMPEG

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace Core {
namespace Media {

namespace {

constexpr int HD_HEIGHT = 720;
constexpr int SWSCALE_OVERRUN_BYTES = 64;

bool isYuv(int format)
{
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return layout && !(layout->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL));
}

} // namespace

bool usesBt709AtHeight(int height)
{
    return height >= HD_HEIGHT;
}

bool usesBt709(const AVFrame *frame)
{
    if (frame->colorspace == AVCOL_SPC_BT709)
        return true;
    if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
        return false;
    return usesBt709AtHeight(frame->height);
}

bool isFullRange(const AVFrame *frame)
{
    if (frame->color_range == AVCOL_RANGE_JPEG)
        return true;

    // formats ffmpeg keeps for full-range pictures from decoders that dont set the range
    switch (frame->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
        return true;
    default:
        return false;
    }
}

RgbScaler::~RgbScaler()
{
    sws_freeContext(context);
}

QImage RgbScaler::allocate(const QSize &size, QImage::Format format)
{
    if (size.isEmpty())
        return {};

    const qsizetype stride = qsizetype(size.width()) * 4;
    auto *pixels = static_cast<uchar *>(av_malloc(size_t(stride) * size_t(size.height()) + SWSCALE_OVERRUN_BYTES));
    if (!pixels)
        return {};
    return QImage(pixels, size.width(), size.height(), stride, format, [](void *owned) { av_free(owned); }, pixels);
}

bool RgbScaler::scale(const AVFrame *frame, QImage &into)
{
    if (into.isNull() || into.depth() != 32)
        return false;
    // writing into a shared image would make Qt copy it into memory with no room to spare
    if (!into.isDetached())
        into = allocate(into.size(), into.format());
    if (into.isNull())
        return false;

    const Source source{ frame->width, frame->height, frame->format, usesBt709(frame), isFullRange(frame), into.size() };
    context = sws_getCachedContext(context, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), into.width(), into.height(), AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context)
        return false;

    if (source != configuredFor) {
        configuredFor = source;
        if (isYuv(frame->format)) {
            const int *coefficients = sws_getCoefficients(source.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            sws_setColorspaceDetails(context, coefficients, source.fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }
    }

    uint8_t *planes[4] = { into.bits(), nullptr, nullptr, nullptr };
    const int strides[4] = { static_cast<int>(into.bytesPerLine()), 0, 0, 0 };
    return sws_scale(context, frame->data, frame->linesize, 0, frame->height, planes, strides) > 0;
}

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
