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

// Untagged video is taken as BT.709 at HD sizes and BT.601 below, as players do, which is also
// what a picture that is sent is made in where its codec cannot say.
[[nodiscard]] bool usesBt709AtHeight(int height);
[[nodiscard]] bool usesBt709(const AVFrame *frame);
[[nodiscard]] bool isFullRange(const AVFrame *frame);

// Scales decoded pictures into 32-bit RGB images.
//
// swscale converts every picture as BT.601 at video range unless told otherwise, whatever the
// frame itself says, so this passes on the frame's own colour description. It follows a
// stream whose size, format or colours change part way through.
class RgbScaler
{
public:
    RgbScaler() = default;
    ~RgbScaler();

    RgbScaler(const RgbScaler &) = delete;
    RgbScaler &operator=(const RgbScaler &) = delete;

    // An image for scale() to write into: aligned for swscale's vector code, with room after
    // the last row, which that code can run past. Null when the memory cannot be had.
    [[nodiscard]] static QImage allocate(const QSize &size, QImage::Format format);

    // Fills `into`, which came from allocate(), at its own size. Alpha is left straight. An
    // image something else still holds is left alone and `into` becomes a new one.
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
