#include "Core/Video/Windows/CardPictures.hpp"

#if defined(Q_OS_WIN)

#include <windows.h>

#include <d3d11.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

AVD3D11VADeviceContext *deviceOf(const AVFrame *card)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWFramesContext *>(card->hw_frames_ctx->data)->device_ctx->hwctx);
}

ID3D11Resource *textureOf(const AVFrame *card)
{
    return reinterpret_cast<ID3D11Resource *>(card->data[0]);
}

UINT sliceOf(const AVFrame *card)
{
    return static_cast<UINT>(reinterpret_cast<quintptr>(card->data[1]));
}

void clearToBlack(AVFrame *card)
{
    AVD3D11VADeviceContext *device = deviceOf(card);
    ID3D11RenderTargetView *view = nullptr;
    if (FAILED(device->device->CreateRenderTargetView(textureOf(card), nullptr, &view)))
        return;
    const float black[4] = { 0, 0, 0, 1 };
    device->lock(device->lock_ctx);
    device->device_context->ClearRenderTargetView(view, black);
    device->unlock(device->lock_ctx);
    view->Release();
}

// `area` of one picture on the graphics card into another of the same card, at `at`.
void copyOnCard(const AVFrame *from, const QRect &area, AVFrame *to, const QPoint &at)
{
    AVD3D11VADeviceContext *device = deviceOf(to);
    const D3D11_BOX box = { static_cast<UINT>(area.left()), static_cast<UINT>(area.top()), 0, static_cast<UINT>(area.left() + area.width()), static_cast<UINT>(area.top() + area.height()), 1 };
    device->lock(device->lock_ctx);
    device->device_context->CopySubresourceRegion(textureOf(to), sliceOf(to), static_cast<UINT>(at.x()), static_cast<UINT>(at.y()), 0, textureOf(from), sliceOf(from), &box);
    device->unlock(device->lock_ctx);
}

} // namespace

AVBufferRef *cardPictures(AVBufferRef *device, const QSize &size)
{
    AVBufferRef *pool = av_hwframe_ctx_alloc(device);
    if (!pool)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_BGRA;
    frames->width = size.width();
    frames->height = size.height();
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(pool) < 0)
        av_buffer_unref(&pool);
    return pool;
}

bool prepareBgra(AVFrame *bgra, const QSize &size)
{
    if (bgra->buf[0] && bgra->format == AV_PIX_FMT_BGRA && bgra->width == size.width() && bgra->height == size.height())
        return av_frame_make_writable(bgra) >= 0;

    av_frame_unref(bgra);
    bgra->format = AV_PIX_FMT_BGRA;
    bgra->width = size.width();
    bgra->height = size.height();
    return av_frame_get_buffer(bgra, 0) >= 0;
}

int readBack(const AVFrame *onCard, AVFrame *memory)
{
    const int format = reinterpret_cast<const AVHWFramesContext *>(onCard->hw_frames_ctx->data)->sw_format;
    const bool fits = memory->buf[0] && memory->format == format && memory->width == onCard->width && memory->height == onCard->height && av_frame_make_writable(memory) >= 0;
    if (!fits) {
        av_frame_unref(memory);
        memory->format = format;
        memory->width = onCard->width;
        memory->height = onCard->height;
        const int made = av_frame_get_buffer(memory, 0);
        if (made < 0)
            return made;
    }
    return av_hwframe_transfer_data(memory, onCard, 0);
}

bool isEmptyPicture(const AVFrame *onCard, AVFrame *scratch)
{
    constexpr int POINTS_ACROSS = 4;
    if (!prepareBgra(scratch, QSize(onCard->width, onCard->height)) || av_hwframe_transfer_data(scratch, onCard, 0) < 0)
        return false;

    for (int row = 0; row < POINTS_ACROSS; row++) {
        const uint8_t *line = scratch->data[0] + (scratch->height * (2 * row + 1) / (2 * POINTS_ACROSS)) * scratch->linesize[0];
        for (int column = 0; column < POINTS_ACROSS; column++) {
            if (line[(scratch->width * (2 * column + 1) / (2 * POINTS_ACROSS)) * 4 + 3] != 0)
                return false;
        }
    }
    return true;
}

bool isSamePictureOnCard(const AVFrame *last, const AVFrame *now)
{
    return last->buf[0] && now->hw_frames_ctx && last->data[0] == now->data[0] && last->data[1] == now->data[1];
}

void keepReferenceTo(AVFrame *last, const AVFrame *now)
{
    av_frame_unref(last);
    av_frame_ref(last, now);
}

QRect wholeOf(const AVFrame *picture)
{
    return QRect(0, 0, picture->width, picture->height);
}

QRect scaledPart(const QRect &part, const QSize &from, const QSize &to)
{
    const QPoint corner(part.left() * to.width() / from.width(), part.top() * to.height() / from.height());
    const QPoint opposite((part.left() + part.width()) * to.width() / from.width(), (part.top() + part.height()) * to.height() / from.height());
    return QRect(corner, QSize(opposite.x() - corner.x(), opposite.y() - corner.y()));
}

bool cutOntoCard(const AVFrame *from, const QRect &area, AVBufferRef *pool, const QPoint &at, AVFrame *to)
{
    if (av_hwframe_get_buffer(pool, to, 0) < 0)
        return false;
    if (area.size() != QSize(to->width, to->height))
        clearToBlack(to);
    copyOnCard(from, area, to, at);
    return true;
}

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
