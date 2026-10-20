#include "Core/Video/Windows/WindowDrawer.hpp"

#if defined(Q_OS_WIN)

#include "Core/Video/CaptureCommon.hpp"
#include "Core/Video/Windows/WindowQueries.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

// PrintWindow's PW_RENDERFULLCONTENT, which the headers keep from a build that also targets
// Windows 7.
constexpr UINT RENDER_FULL_CONTENT = 2;
constexpr int THUMBNAIL_DRAWING_LIMIT_MS = 500;

} // namespace

Bitmap::Bitmap(const QSize &size) : extent(size), memory(CreateCompatibleDC(nullptr))
{
    BITMAPINFO layout = {};
    layout.bmiHeader.biSize = sizeof(layout.bmiHeader);
    layout.bmiHeader.biWidth = size.width();
    // Negative for rows that run from the top down.
    layout.bmiHeader.biHeight = -size.height();
    layout.bmiHeader.biPlanes = 1;
    layout.bmiHeader.biBitCount = 32;
    layout.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(memory, &layout, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap)
        previous = SelectObject(memory, bitmap);
}

Bitmap::~Bitmap()
{
    if (bitmap) {
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
}

QImage shrunk(HDC from, const QRect &area, const QSize &bound)
{
    const QSize size = area.width() > bound.width() || area.height() > bound.height() ? area.size().scaled(bound, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)) : area.size();
    const Bitmap reduced(size);
    if (!reduced.usable())
        return {};

    SetStretchBltMode(reduced.context(), HALFTONE);
    SetBrushOrgEx(reduced.context(), 0, 0, nullptr);
    if (!StretchBlt(reduced.context(), 0, 0, size.width(), size.height(), from, area.x(), area.y(), area.width(), area.height(), SRCCOPY))
        return {};
    GdiFlush();

    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); y++) {
        const quint32 *source = reduced.row(y);
        auto *to = reinterpret_cast<quint32 *>(image.scanLine(y));
        // GDI leaves the byte an image keeps for opacity at zero.
        for (int x = 0; x < size.width(); x++)
            to[x] = source[x] | 0xFF000000u;
    }
    return image;
}

std::shared_ptr<WindowDrawer> WindowDrawer::start(HWND window, bool insideOnly)
{
    auto drawer = std::make_shared<WindowDrawer>(window, insideOnly);
    std::thread([drawer] { drawer->run(); }).detach();
    return drawer;
}

void WindowDrawer::abandon()
{
    {
        const std::lock_guard lock(mutex);
        abandoned = true;
    }
    changed.notify_all();
}

bool WindowDrawer::draw(std::chrono::milliseconds limit, const std::function<bool()> &stillWanted)
{
    std::unique_lock lock(mutex);
    if (drawing)
        return false;

    const QSize size = sizeAsTheWindowMeasures();
    if (size.isEmpty())
        return false;
    if (!bitmap || bitmap->size() != size)
        bitmap = std::make_unique<Bitmap>(size);
    if (!bitmap->usable())
        return false;

    drawing = true;
    changed.notify_all();
    const auto giveUpAt = std::chrono::steady_clock::now() + limit;
    while (drawing && (!stillWanted || stillWanted())) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= giveUpAt)
            break;
        changed.wait_for(lock, std::min<std::chrono::steady_clock::duration>(giveUpAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)), [this] { return !drawing; });
    }
    return !drawing && drew;
}

void WindowDrawer::drawPointer(const QRect &client)
{
    CURSORINFO cursor = {};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
        return;
    const QPoint at(cursor.ptScreenPos.x, cursor.ptScreenPos.y);
    const HWND under = WindowFromPoint(cursor.ptScreenPos);
    if (!client.contains(at) || !under || GetAncestor(under, GA_ROOT) != window)
        return;

    if (cursor.hCursor != hotspotOf) {
        ICONINFO icon = {};
        hotspot = QPoint();
        if (GetIconInfo(cursor.hCursor, &icon)) {
            hotspot = QPoint(static_cast<int>(icon.xHotspot), static_cast<int>(icon.yHotspot));
            DeleteObject(icon.hbmMask);
            if (icon.hbmColor)
                DeleteObject(icon.hbmColor);
        }
        hotspotOf = cursor.hCursor;
    }
    const QSize size = bitmap->size();
    DrawIconEx(bitmap->context(), (at.x() - client.x()) * size.width() / client.width() - hotspot.x(), (at.y() - client.y()) * size.height() / client.height() - hotspot.y(), cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    GdiFlush();
}

QSize WindowDrawer::sizeAsTheWindowMeasures() const
{
    const ThreadScaling asTheWindow(ThreadScaling::of(window));
    RECT area = {};
    if (!(insideOnly ? GetClientRect(window, &area) : GetWindowRect(window, &area)))
        return {};
    return fromNative(area).size();
}

void WindowDrawer::run()
{
    std::unique_lock lock(mutex);
    for (;;) {
        changed.wait(lock, [this] { return drawing || abandoned; });
        if (abandoned)
            return;

        lock.unlock();
        bool printed = false;
        {
            const ThreadScaling asTheWindow(ThreadScaling::of(window));
            printed = PrintWindow(window, bitmap->context(), RENDER_FULL_CONTENT | (insideOnly ? PW_CLIENTONLY : 0)) != FALSE;
            GdiFlush();
        }
        lock.lock();
        drew = printed;
        drawing = false;
        changed.notify_all();
    }
}

QImage windowThumbnail(HWND window, const QSize &bound)
{
    RECT whole = {};
    if (!window || IsIconic(window) || !GetWindowRect(window, &whole) || IsRectEmpty(&whole))
        return {};

    const std::shared_ptr<WindowDrawer> drawer = WindowDrawer::start(window, false);
    const bool drew = drawer->draw(std::chrono::milliseconds(THUMBNAIL_DRAWING_LIMIT_MS));
    drawer->abandon();
    if (!drew)
        return {};

    // What shows of the window, placed in a bitmap that may be at the smaller size a scaled
    // program draws itself at.
    const QRect outer = fromNative(whole);
    const QRect shown = visibleBounds(window).translated(-outer.topLeft()) & QRect(QPoint(0, 0), outer.size());
    const QSize drawn = drawer->drawn().size();
    const QRect inBitmap(shown.x() * drawn.width() / outer.width(), shown.y() * drawn.height() / outer.height(), shown.width() * drawn.width() / outer.width(), shown.height() * drawn.height() / outer.height());
    return inBitmap.isEmpty() ? QImage() : shrunk(drawer->drawn().context(), inBitmap, bound);
}

void copyTopLeft(const Bitmap &from, AVFrame *bgra)
{
    const int columns = std::min(from.size().width(), bgra->width);
    for (int y = 0; y < bgra->height; y++) {
        uint8_t *to = bgra->data[0] + static_cast<qsizetype>(y) * bgra->linesize[0];
        const int copied = y < from.size().height() ? columns : 0;
        if (copied > 0)
            memcpy(to, from.row(y), static_cast<size_t>(copied) * 4);
        memset(to + static_cast<qsizetype>(copied) * 4, 0, static_cast<size_t>(bgra->width - copied) * 4);
    }
}

bool showsTopLeftOf(const Bitmap &from, const AVFrame *bgra)
{
    const int columns = std::min(from.size().width(), bgra->width);
    const int rows = std::min(from.size().height(), bgra->height);
    for (int y = 0; y < rows; y++) {
        if (memcmp(bgra->data[0] + static_cast<qsizetype>(y) * bgra->linesize[0], from.row(y), static_cast<size_t>(columns) * 4) != 0)
            return false;
    }
    return true;
}

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
