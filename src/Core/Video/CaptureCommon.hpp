#pragma once

#include <QCoreApplication>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "Core/Video/CaptureSources.hpp"

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace Core {
namespace Video {

inline constexpr int STALLED_WAIT_MS = 50;
// past the time a picture was due, before a read says nothing new came
inline constexpr int NOTHING_NEW_WAIT_MS = 25;
// graphics card encoders refuse much smaller pictures; a smaller window gets black around it
inline constexpr QSize SMALLEST_WINDOW_PICTURE(160, 120);

inline QString captureText(const char *text)
{
    return QCoreApplication::translate("Acheron::Core::Video::ScreenCapture", text);
}

inline QString kindName(CaptureSource::Kind kind)
{
    return kind == CaptureSource::Kind::Window ? captureText("Window") : captureText("Screen");
}

inline QString numberedScreenName(int number)
{
    return captureText("Screen %1").arg(number);
}

inline QString captureLostText(CaptureSource::Kind kind)
{
    return kind == CaptureSource::Kind::Window ? captureText("The window can no longer be captured (%1).") : captureText("The screen can no longer be captured (%1).");
}

inline QSize evenSize(const QSize &size)
{
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

inline QSize fittedInside(const QSize &size, const QSize &bound)
{
    if (bound.isEmpty() || (size.width() <= bound.width() && size.height() <= bound.height()))
        return evenSize(size);
    return evenSize(size.scaled(bound, Qt::KeepAspectRatio));
}

inline QSize shrunkToFit(const QSize &size, const QSize &bound)
{
    if (size.width() <= bound.width() && size.height() <= bound.height())
        return size;
    return size.scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

inline constexpr int RGB32_BYTES_PER_PIXEL = 4;

inline void makeOpaque(QImage &image)
{
    for (int y = 0; y < image.height(); y++) {
        auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++)
            row[x] |= 0xFF000000u;
    }
}

// into a 32-bit picture: the top left of `from`, and black wherever it doesnt reach
inline void copyTopLeftOnBlack(const uint8_t *from, qsizetype fromStride, const QSize &fromSize, AVFrame *into)
{
    const int columns = std::min(fromSize.width(), into->width);
    for (int y = 0; y < into->height; y++) {
        uint8_t *to = into->data[0] + static_cast<qsizetype>(y) * into->linesize[0];
        const int copied = y < fromSize.height() ? columns : 0;
        if (copied > 0)
            std::memcpy(to, from + static_cast<qsizetype>(y) * fromStride, static_cast<size_t>(copied) * RGB32_BYTES_PER_PIXEL);
        std::memset(to + static_cast<qsizetype>(copied) * RGB32_BYTES_PER_PIXEL, 0, static_cast<size_t>(into->width - copied) * RGB32_BYTES_PER_PIXEL);
    }
}

inline QRect visibleArea(const AVFrame *frame)
{
    return QRect(static_cast<int>(frame->crop_left), static_cast<int>(frame->crop_top), frame->width - static_cast<int>(frame->crop_left + frame->crop_right), frame->height - static_cast<int>(frame->crop_top + frame->crop_bottom));
}

// `area` must lie inside the frame
inline void cropTo(AVFrame *frame, const QRect &area)
{
    frame->crop_left = static_cast<size_t>(area.left());
    frame->crop_top = static_cast<size_t>(area.top());
    frame->crop_right = static_cast<size_t>(frame->width - area.left() - area.width());
    frame->crop_bottom = static_cast<size_t>(frame->height - area.top() - area.height());
}

inline QSize windowPictureSize(const QSize &client)
{
    return evenSize(client).expandedTo(SMALLEST_WINDOW_PICTURE);
}

inline std::chrono::microseconds pictureInterval(int framerate)
{
    return std::chrono::microseconds(1'000'000 / std::max(1, framerate));
}

// waits a little, while there is time left until `retryAt`
inline bool waitedForRetry(std::chrono::steady_clock::time_point retryAt)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= retryAt)
        return false;
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
    return true;
}

// pictures of the screen's own RGB
inline void markColours(AVFrame *picture, bool bt601)
{
    picture->colorspace = bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    picture->color_range = AVCOL_RANGE_MPEG;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
