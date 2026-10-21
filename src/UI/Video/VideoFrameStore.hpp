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

[[nodiscard]] QSize fitted(QSize content, QSize bounds);
[[nodiscard]] bool isUploadable(int format);

// the decoder's thread converts into `back` and swaps it with `front` under the lock, so nothing is
// shared or allocated per frame
class VideoFrameStore : public Core::Media::VideoFrameSink, public std::enable_shared_from_this<VideoFrameStore>
{
public:
    VideoFrameStore(VideoSurface *owner, bool gpu);
    ~VideoFrameStore() override;

    void present(const AVFrame *frame) override;
    bool acceptsHardwareFrames(int format) const override;

#ifdef Q_OS_WIN
    void setCard(std::shared_ptr<CardPicture> picture);
#endif

#ifdef ACHERON_HAVE_DMABUF
    void setCardUsable(bool usable);
    [[nodiscard]] const AVFrame *exportedFrame() const { return exported; }
    void readBack();
#endif

    void setShown(bool value);
    void detach();
    [[nodiscard]] bool hasPicture();
    // a picture left on the card holds one of its decoder's surfaces
    void keepInMemory();
    void setTarget(QSize devicePixels, qreal devicePixelRatio);
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

    template <typename Reader>
    void withFrame(Reader read)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        read(last, serial, displaySize());
    }

private:
    // with the lock held, all of them
    void announce();
    [[nodiscard]] QSize displaySize() const;
    void store(const AVFrame *frame);
    void convert();
    bool storeDownloaded();
    bool bringIntoMemory();
#ifdef Q_OS_WIN
    void requestHandOver();
    // on the worker's thread, without the lock
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
    AVFrame *handing;
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
