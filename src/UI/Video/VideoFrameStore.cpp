#include "VideoFrameStore.hpp"

#include "VideoSurface.hpp"

#ifdef Q_OS_WIN
#include "CardWorker.hpp"
#endif
#ifdef ACHERON_HAVE_DMABUF
#include "DmabufGlBridge.hpp"
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace UI {

QSize fitted(QSize content, QSize bounds)
{
    if (content.isEmpty() || bounds.isEmpty())
        return {};
    return content.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

bool isUploadable(int format)
{
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

VideoFrameStore::VideoFrameStore(VideoSurface *owner, bool gpu) : owner(owner), gpu(gpu), last(av_frame_alloc()), handing(av_frame_alloc()) {}

VideoFrameStore::~VideoFrameStore()
{
    sws_freeContext(normalizer);
    av_frame_free(&last);
    av_frame_free(&handing);
    av_frame_free(&exported);
}

void VideoFrameStore::present(const AVFrame *frame)
{
#ifdef ACHERON_HAVE_DMABUF
    // waits for the card to finish the picture, on the decoder's thread and outside the lock
    AVFrame *handed = frame->format == AV_PIX_FMT_VAAPI ? DmabufGlBridge::exported(frame) : nullptr;
#endif
    std::lock_guard lock(mutex);
#ifdef ACHERON_HAVE_DMABUF
    av_frame_free(&exported);
    exported = handed;
#endif
    if (!owner)
        return;

    store(frame);
    serial++;
#ifdef Q_OS_WIN
    // the surface may have no layer yet to draw it in
    if (last->hw_frames_ctx && card) {
        requestHandOver();
        return;
    }
#endif
    if (!gpu)
        convert();
    announce();
}

#ifdef Q_OS_WIN
void VideoFrameStore::setCard(std::shared_ptr<CardPicture> picture)
{
    std::lock_guard lock(mutex);
    card = std::move(picture);
    hardwareFramesUsable.store(card != nullptr, std::memory_order_relaxed);
    if (!card && bringIntoMemory())
        announce();
    // the frame held may be one the previous card never got to
    if (shown && last->hw_frames_ctx)
        requestHandOver();
}
#endif

#ifdef ACHERON_HAVE_DMABUF
void VideoFrameStore::setCardUsable(bool usable)
{
    hardwareFramesUsable.store(usable, std::memory_order_relaxed);
}

void VideoFrameStore::readBack()
{
    storeDownloaded();
    av_frame_free(&exported);
}
#endif

bool VideoFrameStore::acceptsHardwareFrames(int format) const
{
#ifdef ACHERON_HAVE_DMABUF
    return format == AV_PIX_FMT_VAAPI && hardwareFramesUsable.load(std::memory_order_relaxed);
#else
    return format == AV_PIX_FMT_D3D11 && hardwareFramesUsable.load(std::memory_order_relaxed);
#endif
}

void VideoFrameStore::setShown(bool value)
{
    std::lock_guard lock(mutex);
    shown = value;
#ifdef Q_OS_WIN
    if (shown && last->hw_frames_ctx)
        requestHandOver();
#endif
}

void VideoFrameStore::detach()
{
    std::lock_guard lock(mutex);
    owner = nullptr;
}

bool VideoFrameStore::hasPicture()
{
    std::lock_guard lock(mutex);
    return last->width > 0;
}

void VideoFrameStore::keepInMemory()
{
    std::lock_guard lock(mutex);
    bringIntoMemory();
}

bool VideoFrameStore::storeDownloaded()
{
    AVFrame *downloaded = av_frame_alloc();
    const bool downloadedWhole = downloaded && av_hwframe_transfer_data(downloaded, last, 0) >= 0 && av_frame_copy_props(downloaded, last) >= 0;
    if (downloadedWhole)
        store(downloaded);
    av_frame_free(&downloaded);
    return downloadedWhole;
}

bool VideoFrameStore::bringIntoMemory()
{
    if (!last->hw_frames_ctx || !storeDownloaded())
        return false;
    av_frame_free(&exported);
    serial++;
    if (!gpu)
        convert();
    return true;
}

void VideoFrameStore::setTarget(QSize devicePixels, qreal devicePixelRatio)
{
    std::lock_guard lock(mutex);
    target = devicePixels;
    pixelRatio = devicePixelRatio;
}

void VideoFrameStore::rescale()
{
    std::lock_guard lock(mutex);
    if (!gpu && last->width > 0)
        convert();
}

void VideoFrameStore::forget()
{
    std::lock_guard lock(mutex);
    av_frame_unref(last);
    av_frame_free(&exported);
    front = QImage();
    serial++;
}

QSize VideoFrameStore::pictureSize()
{
    std::lock_guard lock(mutex);
    return fitted(displaySize(), target);
}

void VideoFrameStore::announce()
{
    if (!owner || updatePending)
        return;
    updatePending = true;
    QMetaObject::invokeMethod(owner, [surface = owner] { surface->onFrameArrived(); }, Qt::QueuedConnection);
}

#ifdef Q_OS_WIN
void VideoFrameStore::requestHandOver()
{
    if (!card || !shown || handOverPending)
        return;
    handOverPending = true;
    card->post([self = shared_from_this(), picture = card] { self->handOver(*picture); });
}

void VideoFrameStore::handOver(CardPicture &picture)
{
    {
        std::lock_guard lock(mutex);
        handOverPending = false;
        if (last->hw_frames_ctx)
            av_frame_ref(handing, last);
    }
    if (!handing->hw_frames_ctx)
        return;
    const bool made = picture.render(handing);
    av_frame_unref(handing);

    std::lock_guard lock(mutex);
    if (!made) {
        hardwareFramesUsable.store(false, std::memory_order_relaxed);
        bringIntoMemory();
    }
    announce();
}
#endif

QSize VideoFrameStore::displaySize() const
{
    if (last->width <= 0 || last->height <= 0)
        return {};
    const AVRational aspect = last->sample_aspect_ratio;
    if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
        return QSize(static_cast<int>(static_cast<qint64>(last->width) * aspect.num / aspect.den), last->height);
    return QSize(last->width, last->height);
}

void VideoFrameStore::store(const AVFrame *frame)
{
    av_frame_unref(last);
    if (!gpu || isUploadable(frame->format) || frame->hw_frames_ctx) {
        av_frame_ref(last, frame);
        return;
    }

    last->format = AV_PIX_FMT_YUV420P;
    last->width = frame->width;
    last->height = frame->height;
    if (av_frame_get_buffer(last, 0) < 0) {
        av_frame_unref(last);
        return;
    }
    normalizer = sws_getCachedContext(normalizer, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
    if (!normalizer) {
        av_frame_unref(last);
        return;
    }
    sws_scale(normalizer, frame->data, frame->linesize, 0, frame->height, last->data, last->linesize);
    last->colorspace = frame->colorspace;
    last->color_range = frame->color_range;
    last->sample_aspect_ratio = frame->sample_aspect_ratio;
}

void VideoFrameStore::convert()
{
    const QSize size = fitted(displaySize(), target);
    if (size.isEmpty())
        return;

    if (back.size() != size)
        back = Core::Media::RgbScaler::allocate(size, QImage::Format_RGB32);
    if (!scaler.scale(last, back))
        return;
    back.setDevicePixelRatio(pixelRatio);
    back.swap(front);
}

} // namespace UI
} // namespace Acheron
