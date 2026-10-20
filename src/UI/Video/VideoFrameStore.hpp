#pragma once

#include <QImage>
#include <QSize>

#include <atomic>
#include <memory>
#include <mutex>

#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoDecoder.hpp"

struct AVFrame;
struct SwsContext;

namespace Acheron {
namespace UI {

class CardPicture;
class VideoSurface;

// `content` made as large as fits in `bounds`, its shape kept. Empty when either is.
[[nodiscard]] QSize fitted(QSize content, QSize bounds);
// The layouts the shader reads directly: 8-bit 4:2:0, planar or with interleaved chroma.
[[nodiscard]] bool isUploadable(int format);

// Holds the latest decoded frame for a surface. For the software path it also converts each
// frame to RGB at the widget's size: the decoder's thread writes into `back` and swaps it with
// `front`, and the widget only ever reads `front`, under the same lock, so neither image is
// shared and none is allocated per frame. A frame still on the graphics card is passed to the
// worker that turns it into a texture, and the widget hears of it when that is done; a VAAPI
// one is kept with its DMA-BUFs, for the widget to draw from.
class VideoFrameStore : public Core::Media::VideoFrameSink, public std::enable_shared_from_this<VideoFrameStore>
{
public:
    VideoFrameStore(VideoSurface *owner, bool gpu);
    ~VideoFrameStore() override;

    void present(const AVFrame *frame) override;
    bool acceptsHardwareFrames(int format) const override;

#ifdef Q_OS_WIN
    // Set by the layer that draws once it can draw the card's pictures, and cleared when it
    // no longer can.
    void setCard(std::shared_ptr<CardPicture> picture);
#endif

#ifdef ACHERON_HAVE_DMABUF
    // Set by the layer that draws once it can draw the card's pictures, and cleared when it
    // no longer can.
    void setCardUsable(bool usable);
    // The frame as DMA-BUFs, when it is one from the card that could be had so. For the reader
    // in withFrame().
    [[nodiscard]] const AVFrame *exportedFrame() const { return exported; }
    // For the reader in withFrame(), when the frame is on the card and cannot be drawn from
    // there: it is brought into memory as the decoder would have brought it.
    void readBack();
#endif

    // Whether the surface is on screen. The card's pictures are not made ready for one that
    // is not.
    void setShown(bool value);
    void detach();
    [[nodiscard]] bool hasPicture();
    // A picture left on the card holds its decoder's surfaces there. One that is not being
    // shown is kept in memory instead.
    void keepInMemory();
    void setTarget(QSize devicePixels, qreal devicePixelRatio);
    // Redraws the last frame at the current size, for when no new frame is coming to do it.
    void rescale();
    void forget();
    [[nodiscard]] QSize pictureSize();

    template <typename Painter>
    void withFront(Painter paint)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        paint(front);
    }

    // The latest frame and a number that changes whenever it does. The frame has no picture
    // (zero width) when there is nothing to show.
    template <typename Reader>
    void withFrame(Reader read)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        read(last, serial, displaySize());
    }

private:
    // With the lock held, all of them.
    void announce();
    [[nodiscard]] QSize displaySize() const;
    void store(const AVFrame *frame);
    void convert();
    // The frame held, from the card into memory, as a new picture. False when it could not be.
    bool bringIntoMemory();
#ifdef Q_OS_WIN
    // One request covers every frame that arrives before the worker gets to it.
    void requestHandOver();
    // On the worker's thread, without the lock.
    void handOver(CardPicture &picture);
#endif

    std::mutex mutex;
    VideoSurface *owner;
    bool gpu;
    bool shown = false;
    std::atomic<bool> hardwareFramesUsable{ false };
#ifdef Q_OS_WIN
    std::shared_ptr<CardPicture> card;
    bool handOverPending = false;
#endif
    AVFrame *last;
    // The worker's own reference to the frame it is turning into a texture.
    AVFrame *handing;
    // `last` as DMA-BUFs, when it is a VAAPI frame.
    AVFrame *exported = nullptr;
    quint64 serial = 0;
    Core::Media::RgbScaler scaler;
    SwsContext *normalizer = nullptr;
    QSize target;
    qreal pixelRatio = 1.0;
    QImage front;
    QImage back;
    bool updatePending = false;
};

} // namespace UI
} // namespace Acheron
