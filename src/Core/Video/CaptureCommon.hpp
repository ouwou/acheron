#pragma once

#include <QCoreApplication>
#include <QSize>
#include <QString>

#include <algorithm>
#include <chrono>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace Core {
namespace Video {

// What the platforms' screen and window capture have in common, for their own use.

using CaptureClock = std::chrono::steady_clock;

// How long a read waits when there is nothing to read for a while.
inline constexpr int STALLED_WAIT_MS = 50;
// How long a read waits past the time a picture was due before it says nothing new has come.
inline constexpr int NOTHING_NEW_WAIT_MS = 25;
// A window smaller than this is sent with black around it: encoders on graphics cards turn
// down pictures much smaller.
inline constexpr QSize SMALLEST_WINDOW_PICTURE(160, 120);

// The capture's texts, which say what went wrong to whoever shares.
inline QString captureText(const char *text)
{
    return QCoreApplication::translate("Acheron::Core::Video::ScreenCapture", text);
}

inline QSize evenSize(const QSize &size)
{
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

// What is captured of a window whose inside measures `client`, before any scaling.
inline QSize windowPictureSize(const QSize &client)
{
    return evenSize(client).expandedTo(SMALLEST_WINDOW_PICTURE);
}

inline std::chrono::microseconds pictureInterval(int framerate)
{
    return std::chrono::microseconds(1'000'000 / std::max(1, framerate));
}

// True, after waiting a little of it, while there is time left until `retryAt`.
inline bool waitedForRetry(CaptureClock::time_point retryAt)
{
    const auto now = CaptureClock::now();
    if (now >= retryAt)
        return false;
    std::this_thread::sleep_for(std::min<CaptureClock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
    return true;
}

// The description the capture gives its pictures, which are of the screen's own RGB.
inline void markColours(AVFrame *picture, bool bt601)
{
    picture->colorspace = bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    picture->color_range = AVCOL_RANGE_MPEG;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
