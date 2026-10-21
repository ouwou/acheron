#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QImage>
#include <QSize>

extern "C" {
#include <libavutil/frame.h>
}

struct SwsContext;

namespace Acheron {
namespace Core {
namespace Media {

// as players do
[[nodiscard]] bool usesBt709AtHeight(int height);
[[nodiscard]] inline bool encodesAsBt601(int height, bool bt601Only)
{
    return bt601Only || !usesBt709AtHeight(height);
}
[[nodiscard]] bool usesBt709(const AVFrame *frame);
[[nodiscard]] bool isFullRange(const AVFrame *frame);

// swscale converts as BT.601 video range unless told otherwise, whatever the frame says
class RgbScaler
{
public:
    RgbScaler() = default;
    ~RgbScaler();

    RgbScaler(const RgbScaler &) = delete;
    RgbScaler &operator=(const RgbScaler &) = delete;

    // aligned for swscale's vector code, with room after the last row, which that code can run past
    [[nodiscard]] static QImage allocate(const QSize &size, QImage::Format format);

    // `into` must come from allocate()
    bool scale(const AVFrame *frame, QImage &into);

private:
    struct Source
    {
        int width = 0;
        int height = 0;
        int format = -1;
        bool bt709 = false;
        bool fullRange = false;
        QSize scaledTo;

        bool operator==(const Source &) const = default;
    };

    SwsContext *context = nullptr;
    Source configuredFor;
};

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
