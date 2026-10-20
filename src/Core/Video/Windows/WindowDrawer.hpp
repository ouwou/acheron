#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <windows.h>

struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

// A 32-bit bitmap GDI draws into, its rows running from the top down.
class Bitmap
{
public:
    explicit Bitmap(const QSize &size);
    ~Bitmap();

    Bitmap(const Bitmap &) = delete;
    Bitmap &operator=(const Bitmap &) = delete;

    [[nodiscard]] bool usable() const { return bitmap != nullptr; }
    [[nodiscard]] HDC context() const { return memory; }
    [[nodiscard]] QSize size() const { return extent; }
    [[nodiscard]] const quint32 *row(int y) const { return static_cast<const quint32 *>(bits) + static_cast<qsizetype>(y) * extent.width(); }

private:
    QSize extent;
    HDC memory;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void *bits = nullptr;
};

// Has a window draw itself into a bitmap, whatever is on top of it on the screen. PrintWindow
// is called on a thread of its own because it does not come back from a program that has
// stopped answering; a drawer left waiting for one is abandoned, and goes when the call returns.
class WindowDrawer
{
public:
    // `insideOnly` draws what is inside the window's frame and not the frame.
    static std::shared_ptr<WindowDrawer> start(HWND window, bool insideOnly);

    WindowDrawer(HWND window, bool insideOnly) : window(window), insideOnly(insideOnly) {}

    void abandon();

    // False when the window would not draw or had not within `limit`, or is no longer wanted,
    // in which case what it draws later is thrown away.
    bool draw(std::chrono::milliseconds limit, const std::function<bool()> &stillWanted = {});

    // What the last successful draw left, until the next.
    [[nodiscard]] const Bitmap &drawn() const { return *bitmap; }

    // PrintWindow leaves the pointer out. It belongs in the picture while it is over the inside
    // of the window with nothing else in between.
    void drawPointer(const QRect &client);

private:
    // A program Windows scales for the screen measures, and draws itself, smaller than it shows.
    [[nodiscard]] QSize sizeAsTheWindowMeasures() const;
    void run();

    const HWND window;
    const bool insideOnly;
    std::mutex mutex;
    std::condition_variable changed;
    // Only the thread that is not drawing touches the bitmap.
    std::unique_ptr<Bitmap> bitmap;
    bool drawing = false;
    bool drew = false;
    bool abandoned = false;
    HCURSOR hotspotOf = nullptr;
    QPoint hotspot;
};

// `area` of what `from` holds, made no larger than `bound`.
[[nodiscard]] QImage shrunk(HDC from, const QRect &area, const QSize &bound);
// A still of the whole window as it draws itself, no larger than `bound`. Null when it would not.
[[nodiscard]] QImage windowThumbnail(HWND window, const QSize &bound);

// The bitmap from its top left corner into `bgra`, with black where it does not reach.
void copyTopLeft(const Bitmap &from, AVFrame *bgra);
// Whether `bgra`, which copyTopLeft filled from a bitmap of this size, still shows what the
// bitmap does.
[[nodiscard]] bool showsTopLeftOf(const Bitmap &from, const AVFrame *bgra);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
