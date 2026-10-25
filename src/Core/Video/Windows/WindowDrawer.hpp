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

// draws the pointer where it shows over `area` onto `onto`, which pictures `area` at `size`
class PointerPainter
{
public:
    void paint(HDC onto, const QSize &size, const QRect &area, HWND onlyOver = nullptr);

private:
    HCURSOR hotspotOf = nullptr;
    QPoint hotspot;
};

// PrintWindow never returns from a hung program, so it runs on its own thread; an abandoned drawer goes
// when the call returns
class WindowDrawer
{
public:
    static std::shared_ptr<WindowDrawer> start(HWND window, bool insideOnly);

    WindowDrawer(HWND window, bool insideOnly) : window(window), insideOnly(insideOnly) {}

    void abandon();

    bool draw(std::chrono::milliseconds limit, const std::function<bool()> &stillWanted = {});

    [[nodiscard]] const Bitmap &drawn() const { return *bitmap; }

    // PrintWindow leaves the pointer out
    void drawPointer(const QRect &client);

private:
    // a program Windows scales for the screen measures, and draws itself, smaller than it shows
    [[nodiscard]] QSize sizeAsTheWindowMeasures() const;
    void run();

    const HWND window;
    const bool insideOnly;
    std::mutex mutex;
    std::condition_variable changed;
    // only the thread that isnt drawing touches it
    std::unique_ptr<Bitmap> bitmap;
    bool drawing = false;
    bool drew = false;
    bool abandoned = false;
    PointerPainter pointer;
};

[[nodiscard]] QImage shrunk(HDC from, const QRect &area, const QSize &bound);
[[nodiscard]] QImage windowThumbnail(HWND window, const QSize &bound);

void copyTopLeft(const Bitmap &from, AVFrame *bgra);
[[nodiscard]] bool showsTopLeftOf(const Bitmap &from, const AVFrame *bgra);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
