#include "VideoFrameStore.hpp"

#include "VideoSurface.hpp"

#ifdef Q_OS_WIN
#include "CardWorker.hpp"
#endif

#include <QTransform>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace UI {

namespace {

bool isUploadable(int format)
{
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

} // namespace

QSize fitted(QSize content, QSize bounds)
{
    if (content.isEmpty() || bounds.isEmpty())
        return {};
    return content.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

VideoFrameStore::VideoFrameStore(VideoSurface *owner, bool gpu) : owner(owner), gpu(gpu) {}

VideoFrameStore::~VideoFrameStore()
{
    sws_freeContext(normalizer);
}

void VideoFrameStore::present(const AVFrame *frame)
{
#ifdef ACHERON_HAVE_DMABUF
    // blocks until the picture is decoded, so before the lock
    Core::Media::FramePointer handed;
    if (frame->format == AV_PIX_FMT_VAAPI)
        handed = DmabufGlBridge::exported(frame);
#endif
    std::lock_guard lock(mutex);
#ifdef ACHERON_HAVE_DMABUF
    exported = std::move(handed);
#endif
    if (!owner)
        return;

    store(frame);
    serial++;
#ifdef Q_OS_WIN
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
    exported.reset();
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
    const Core::Media::FramePointer downloaded(av_frame_alloc());
    const bool downloadedWhole = downloaded && av_hwframe_transfer_data(downloaded.get(), last.get(), 0) >= 0 && av_frame_copy_props(downloaded.get(), last.get()) >= 0;
    if (downloadedWhole)
        store(downloaded.get());
    return downloadedWhole;
}

bool VideoFrameStore::bringIntoMemory()
{
    if (!last->hw_frames_ctx || !storeDownloaded())
        return false;
    exported.reset();
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
    av_frame_unref(last.get());
    exported.reset();
    front = QImage();
    turned = QImage();
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
            av_frame_ref(handing.get(), last.get());
    }
    if (!handing->hw_frames_ctx)
        return;
    const bool made = picture.render(handing.get());
    av_frame_unref(handing.get());

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
    QSize size(last->width, last->height);
    const AVRational aspect = last->sample_aspect_ratio;
    if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
        size.setWidth(static_cast<int>(static_cast<qint64>(last->width) * aspect.num / aspect.den));
    return Core::Media::quarterTurnsOf(last.get()) % 2 ? size.transposed() : size;
}

void VideoFrameStore::store(const AVFrame *frame)
{
    av_frame_unref(last.get());
    if (!gpu || isUploadable(frame->format) || frame->hw_frames_ctx) {
        av_frame_ref(last.get(), frame);
        return;
    }

    last->format = AV_PIX_FMT_YUV420P;
    last->width = frame->width;
    last->height = frame->height;
    if (av_frame_get_buffer(last.get(), 0) < 0) {
        av_frame_unref(last.get());
        return;
    }
    normalizer = sws_getCachedContext(normalizer, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
    if (!normalizer) {
        av_frame_unref(last.get());
        return;
    }
    sws_scale(normalizer, frame->data, frame->linesize, 0, frame->height, last->data, last->linesize);
    av_frame_copy_props(last.get(), frame);
}

void VideoFrameStore::convert()
{
    const QSize size = fitted(displaySize(), target);
    if (size.isEmpty())
        return;

    const int quarterTurns = Core::Media::quarterTurnsOf(last.get());
    const QSize beforeTurning = quarterTurns % 2 ? size.transposed() : size;
    if (back.size() != beforeTurning)
        back = Core::Media::RgbScaler::allocate(beforeTurning, QImage::Format_RGB32);
    if (!scaler.scale(last.get(), back))
        return;
    back.setDevicePixelRatio(pixelRatio);
    back.swap(front);
    turned = quarterTurns != 0 ? front.transformed(QTransform().rotate(quarterTurns * 90.0)) : QImage();
    turned.setDevicePixelRatio(pixelRatio);
}

} // namespace UI
} // namespace Acheron
