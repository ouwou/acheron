#include "Core/Video/ScreenCapture.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Video/CardScaler.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QOperatingSystemVersion>
#include <QSettings>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Capture is ffmpeg's desktop duplication and graphics capture sources, which are in libavfilter
// and Windows only.
#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_SCREEN_CAPTURE
#endif

#ifdef ACHERON_SCREEN_CAPTURE
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include "Core/Video/PictureConverter.hpp"
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_LINUX_SCREEN_CAPTURE
#endif

namespace Acheron {
namespace Core {
namespace Video {

namespace {

const char *const CAPTURE_METHOD_KEY = "video/capture_method";
const QLatin1String AUTO_METHOD("auto");
const QLatin1String GRAPHICS_CAPTURE_METHOD("graphics_capture");
const QLatin1String DESKTOP_DUPLICATION_METHOD("desktop_duplication");
const QLatin1String PORTAL_METHOD("portal");
const QLatin1String X11_METHOD("x11");

} // namespace

QSize encodeSizeFor(const QSize &source, int maxHeight)
{
    QSize size = source;
    const QSize bound(maxHeight * 16 / 9, maxHeight);
    if (maxHeight > 0 && (source.width() > bound.width() || source.height() > bound.height()))
        size = source.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

CaptureMethod captureMethod()
{
    const QString stored = QSettings().value(CAPTURE_METHOD_KEY).toString();
    if (stored == GRAPHICS_CAPTURE_METHOD)
        return CaptureMethod::GraphicsCapture;
    if (stored == DESKTOP_DUPLICATION_METHOD)
        return CaptureMethod::DesktopDuplication;
    if (stored == PORTAL_METHOD)
        return CaptureMethod::Portal;
    if (stored == X11_METHOD)
        return CaptureMethod::X11;
    return CaptureMethod::Auto;
}

void setCaptureMethod(CaptureMethod method)
{
    QLatin1String stored = AUTO_METHOD;
    if (method == CaptureMethod::GraphicsCapture)
        stored = GRAPHICS_CAPTURE_METHOD;
    else if (method == CaptureMethod::DesktopDuplication)
        stored = DESKTOP_DUPLICATION_METHOD;
    else if (method == CaptureMethod::Portal)
        stored = PORTAL_METHOD;
    else if (method == CaptureMethod::X11)
        stored = X11_METHOD;
    QSettings().setValue(CAPTURE_METHOD_KEY, QString(stored));
}

#ifndef ACHERON_LINUX_SCREEN_CAPTURE
bool isChosenByTheDesktop(const CaptureSource &)
{
    return false;
}

bool isHandedOverByTheDesktop(const CaptureSource &)
{
    return false;
}

void chooseSource(const CaptureSource &source, QObject *, std::function<void(const CaptureSource &, const QString &)> chosen)
{
    chosen(source, QString());
}

void releaseSource(const CaptureSource &)
{
}
#endif

bool stillPictureIsDue(qint64 msSinceLastPicture, bool keyFrameWaitedFor, int framerate)
{
    return msSinceLastPicture >= STILL_PICTURE_INTERVAL_MS || (keyFrameWaitedFor && msSinceLastPicture >= 1000 / std::max(1, framerate));
}

bool windowIsCovered(const QList<StackedWindow> &topDown, int target)
{
    if (target < 0 || target >= topDown.size())
        return true;
    const StackedWindow &window = topDown[target];
    if (window.bounds.isEmpty() || window.minimised || window.cloaked)
        return true;

    for (int i = 0; i < target; i++) {
        const StackedWindow &above = topDown[i];
        if (!above.visible || above.minimised)
            continue;
        if (above.livePreview)
            return true;
        if (above.overlay || above.cloaked || above.tabProxy)
            continue;
        if (above.bounds.intersects(window.bounds))
            return true;
    }
    return false;
}

#ifdef ACHERON_SCREEN_CAPTURE

namespace {

using Clock = std::chrono::steady_clock;

const QLatin1String SCREEN_ID_PREFIX("screen:");
const QLatin1String WINDOW_ID_PREFIX("window:");
constexpr int STALLED_WAIT_MS = 50;
// How long a read of graphics capture waits past the time a picture was due before it says
// that nothing new has come.
constexpr int NOTHING_NEW_WAIT_MS = 25;
// A window smaller than this is sent with black around it: encoders on graphics cards turn
// down pictures much smaller.
constexpr QSize SMALLEST_WINDOW_PICTURE(160, 120);
// How far a window may reach past the edge of its screen and still be cut out of it, as the
// official client has it.
constexpr int REACH_PAST_SCREEN = 15;
// How long after a window was last seen hidden, or its capture begun, its pictures are read
// back and looked at, unless one with something in it comes sooner. Only a window that has
// nothing to show wherever it is looked at is held up for all of it.
constexpr int EMPTY_PICTURES_FOR_MS = 3000;
constexpr int THUMBNAIL_DRAWING_LIMIT_MS = 500;

// DwmGetWindowAttribute's DWMWA_EXTENDED_FRAME_BOUNDS and DWMWA_CLOAKED, and PrintWindow's
// PW_RENDERFULLCONTENT, which the headers keep from a build that also targets Windows 7.
constexpr DWORD FRAME_BOUNDS_ATTRIBUTE = 9;
constexpr DWORD CLOAKED_ATTRIBUTE = 14;
constexpr UINT RENDER_FULL_CONTENT = 2;

// Set when graphics capture turned out to be missing something only a capture finds out.
std::atomic<bool> graphicsCaptureRuledOut{ false };

using Media::FramePointer;

// For as long as one lives, its thread measures the desktop the way it asked to.
class ThreadScaling
{
public:
    explicit ThreadScaling(DPI_AWARENESS_CONTEXT wanted)
    {
        if (setAwareness && wanted)
            previous = setAwareness(wanted);
    }
    ~ThreadScaling()
    {
        if (previous)
            setAwareness(previous);
    }

    ThreadScaling(const ThreadScaling &) = delete;
    ThreadScaling &operator=(const ThreadScaling &) = delete;

    // How `window` measures, which is how it draws itself.
    [[nodiscard]] static DPI_AWARENESS_CONTEXT of(HWND window) { return windowAwareness ? windowAwareness(window) : nullptr; }

private:
    // Neither is there before Windows 10 1607.
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using WindowAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(HWND);
    static inline const SetAwareness setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    static inline const WindowAwareness windowAwareness = reinterpret_cast<WindowAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext")));
    DPI_AWARENESS_CONTEXT previous = nullptr;
};

// Windows scales the coordinates it gives a thread that has not said it understands display
// scaling, and a thread of a program without a window has not.
class UnscaledCoordinates : public ThreadScaling
{
public:
    UnscaledCoordinates() : ThreadScaling(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) {}
};

struct Monitor
{
    int adapter = 0;
    // Its place among the adapter's outputs, attached to the desktop or not, which is how
    // ddagrab counts them.
    int output = 0;
    HMONITOR handle = nullptr;
    QRect geometry;
    // Turned a quarter, in which case Windows hands its picture over lying on its side.
    bool onItsSide = false;
    bool turned = false;
};

std::vector<Monitor> attachedMonitors()
{
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    static const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));

    std::vector<Monitor> found;
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return found;

    IDXGIAdapter *adapter = nullptr;
    for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters(adapterIndex, &adapter)); adapterIndex++) {
        IDXGIOutput *output = nullptr;
        for (UINT outputIndex = 0; SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)); outputIndex++) {
            DXGI_OUTPUT_DESC description;
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            // The mode is in the screen's own pixels whatever the calling thread understands,
            // which the output's desktop coordinates are not.
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop && EnumDisplaySettingsW(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                Monitor monitor;
                monitor.adapter = static_cast<int>(adapterIndex);
                monitor.output = static_cast<int>(outputIndex);
                monitor.handle = description.Monitor;
                monitor.geometry = QRect(mode.dmPosition.x, mode.dmPosition.y, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight));
                monitor.onItsSide = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
                monitor.turned = monitor.onItsSide || description.Rotation == DXGI_MODE_ROTATION_ROTATE180;
                found.push_back(monitor);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

const Monitor *numberedMonitor(const std::vector<Monitor> &monitors, const QString &sourceId)
{
    bool numbered = false;
    const int number = sourceId.mid(SCREEN_ID_PREFIX.size()).toInt(&numbered);
    const bool listed = sourceId.startsWith(SCREEN_ID_PREFIX) && numbered && number >= 0 && number < static_cast<int>(monitors.size());
    return listed ? &monitors[static_cast<size_t>(number)] : nullptr;
}

using Media::describeError;

QSize evenSize(const QSize &size)
{
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

QRect fromNative(const RECT &rect)
{
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

HWND windowOf(const QString &sourceId)
{
    bool numbered = false;
    const qulonglong handle = sourceId.mid(WINDOW_ID_PREFIX.size()).toULongLong(&numbered);
    return sourceId.startsWith(WINDOW_ID_PREFIX) && numbered ? reinterpret_cast<HWND>(static_cast<quintptr>(handle)) : nullptr;
}

HRESULT windowAttribute(HWND window, DWORD attribute, void *value, DWORD size)
{
    using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = reinterpret_cast<GetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
    return getAttribute ? getAttribute(window, attribute, value, size) : E_INVALIDARG;
}

// Kept from the screen by the program (1) or by the shell (2), as the official client counts
// it. A Windows that does not know the question calls the argument invalid, and has no cloaks.
bool isCloaked(HWND window)
{
    DWORD cloak = 0;
    const HRESULT asked = windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak));
    return SUCCEEDED(asked) ? cloak == 1 || cloak == 2 : asked != E_INVALIDARG;
}

// The window as it shows on the screen. GetWindowRect also counts the invisible borders
// Windows 10 puts around a window that can be resized.
QRect visibleBounds(HWND window)
{
    RECT bounds = {};
    if (FAILED(windowAttribute(window, FRAME_BOUNDS_ATTRIBUTE, &bounds, sizeof(bounds))) || IsRectEmpty(&bounds)) {
        if (!GetWindowRect(window, &bounds))
            return {};
    }
    return fromNative(bounds);
}

// The inside of the window's frame, which is what is shared of it.
QRect clientOnScreen(HWND window)
{
    RECT client = {};
    POINT corner = {};
    if (GetClientRect(window, &client) && ClientToScreen(window, &corner))
        return QRect(corner.x, corner.y, client.right, client.bottom);
    return GetWindowRect(window, &client) ? fromNative(client) : QRect();
}

bool hasClass(HWND window, const wchar_t *name)
{
    wchar_t found[64] = {};
    return GetClassNameW(window, found, 64) > 0 && wcscmp(found, name) == 0;
}

struct WindowStack
{
    HWND target = nullptr;
    HWND foreground = nullptr;
    QRect targetClient;
    QList<StackedWindow> topDown;
    bool reachedTarget = false;
    // Every window EnumWindows lists, below the target as well, to tell the shell's from.
    std::vector<HWND> ordinary;
};

BOOL CALLBACK stackDownToTarget(HWND window, LPARAM parameter)
{
    auto *stack = reinterpret_cast<WindowStack *>(parameter);
    stack->ordinary.push_back(window);
    if (stack->reachedTarget)
        return TRUE;

    StackedWindow each;
    each.visible = IsWindowVisible(window);
    each.minimised = IsIconic(window);
    if (window == stack->target) {
        each.bounds = stack->targetClient;
        each.cloaked = isCloaked(window);
        stack->topDown.append(each);
        stack->reachedTarget = true;
        return TRUE;
    }

    if (each.visible && !each.minimised) {
        const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        each.bounds = visibleBounds(window);
        each.cloaked = isCloaked(window);
        // A window that holds the foreground has come forward, whatever its styles say.
        each.overlay = (extended & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0 && window != stack->foreground;
        each.tabProxy = hasClass(window, L"Windows.Internal.Shell.TabProxyWindow");
        each.livePreview = (extended & WS_EX_TOPMOST) && hasClass(window, L"LivePreview");
    }
    stack->topDown.append(each);
    return TRUE;
}

// Task View, the Start menu, the notification centre and the like are windows of the shell on
// layers above every ordinary window. EnumWindows leaves them out and FindWindowEx does not,
// so they are the ones only the second finds. They exist all the time and are cloaked until
// shown, and the official client's walk never sees them: with one of them open it shares
// whatever part of it lies over the window.
bool shellSurfaceCovers(std::vector<HWND> ordinary, const QRect &client)
{
    std::sort(ordinary.begin(), ordinary.end());
    for (HWND window = FindWindowExW(nullptr, nullptr, nullptr, nullptr); window; window = FindWindowExW(nullptr, window, nullptr, nullptr)) {
        if (std::binary_search(ordinary.begin(), ordinary.end(), window) || !IsWindowVisible(window) || IsIconic(window))
            continue;

        DWORD cloak = 0;
        if (FAILED(windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak))) || cloak != 0)
            continue;
        if (visibleBounds(window).intersects(client))
            return true;
    }
    return false;
}

enum class ShellSurfaces {
    LookedFor,
    // For a first look that a second, after the screen is read, decides over.
    LeftForLater,
};

bool isCoveredOnScreen(HWND window, const QRect &client, ShellSurfaces shellSurfaces)
{
    WindowStack stack;
    stack.target = window;
    stack.foreground = GetForegroundWindow();
    stack.targetClient = client;
    EnumWindows(stackDownToTarget, reinterpret_cast<LPARAM>(&stack));
    if (!stack.reachedTarget || windowIsCovered(stack.topDown, static_cast<int>(stack.topDown.size()) - 1))
        return true;
    return shellSurfaces == ShellSurfaces::LookedFor && shellSurfaceCovers(std::move(stack.ordinary), client);
}

// A window a user would call one: on the screen or minimised, with a title, and not a tool
// window, an overlay or the desktop.
bool isShareable(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() || !IsWindowVisible(window) || window == GetShellWindow() || isCloaked(window))
        return false;

    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) || ((extended & WS_EX_NOACTIVATE) && !(extended & WS_EX_APPWINDOW)))
        return false;
    return IsIconic(window) || !visibleBounds(window).isEmpty();
}

// A store app's window belongs to a host process, and the app, which is what plays its sound,
// to the process of the window inside it.
quint32 processShownIn(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (hasClass(window, L"ApplicationFrameWindow")) {
        DWORD inner = 0;
        const HWND core = FindWindowExW(window, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        if (core && GetWindowThreadProcessId(core, &inner) && inner)
            return inner;
    }
    return process;
}

BOOL CALLBACK listShareable(HWND window, LPARAM list)
{
    wchar_t title[512] = {};
    if (!isShareable(window) || GetWindowTextW(window, title, 512) <= 0)
        return TRUE;

    CaptureSource source;
    source.id = WINDOW_ID_PREFIX + QString::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(window)));
    source.kind = CaptureSource::Kind::Window;
    source.name = QString::fromWCharArray(title);
    source.processId = processShownIn(window);
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!IsIconic(window))
        source.geometry = visibleBounds(window);
    else if (GetWindowPlacement(window, &placement))
        source.geometry = fromNative(placement.rcNormalPosition);
    reinterpret_cast<QList<CaptureSource> *>(list)->append(source);
    return TRUE;
}

// A 32-bit bitmap GDI draws into, its rows running from the top down.
class Bitmap
{
public:
    explicit Bitmap(const QSize &size) : extent(size), memory(CreateCompatibleDC(nullptr))
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

    ~Bitmap()
    {
        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
    }

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

// `area` of what `from` holds, made no larger than `bound`.
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

// Has a window draw itself into a bitmap, whatever is on top of it on the screen. PrintWindow
// is called on a thread of its own because it does not come back from a program that has
// stopped answering; a drawer left waiting for one is abandoned, and goes when the call returns.
class WindowDrawer
{
public:
    static std::shared_ptr<WindowDrawer> start(HWND window, bool insideOnly)
    {
        auto drawer = std::make_shared<WindowDrawer>(window, insideOnly);
        std::thread([drawer] { drawer->run(); }).detach();
        return drawer;
    }

    WindowDrawer(HWND window, bool insideOnly) : window(window), insideOnly(insideOnly) {}

    void abandon()
    {
        {
            const std::lock_guard lock(mutex);
            abandoned = true;
        }
        changed.notify_all();
    }

    // False when the window would not draw or had not within `limit`, or is no longer wanted,
    // in which case what it draws later is thrown away.
    bool draw(std::chrono::milliseconds limit, const std::function<bool()> &stillWanted = {})
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

    // What the last successful draw left, until the next.
    [[nodiscard]] const Bitmap &drawn() const { return *bitmap; }

    // PrintWindow leaves the pointer out. It belongs in the picture while it is over the inside
    // of the window with nothing else in between.
    void drawPointer(const QRect &client)
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

private:
    // A program Windows scales for the screen measures, and draws itself, smaller than it shows.
    QSize sizeAsTheWindowMeasures() const
    {
        const ThreadScaling asTheWindow(ThreadScaling::of(window));
        RECT area = {};
        if (!(insideOnly ? GetClientRect(window, &area) : GetWindowRect(window, &area)))
            return {};
        return fromNative(area).size();
    }

    void run()
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

bool prepareBgra(AVFrame *frame, const QSize &size)
{
    if (frame->buf[0] && frame->format == AV_PIX_FMT_BGRA && frame->width == size.width() && frame->height == size.height())
        return av_frame_make_writable(frame) >= 0;

    av_frame_unref(frame);
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = size.width();
    frame->height = size.height();
    return av_frame_get_buffer(frame, 0) >= 0;
}

bool showsItself(HWND window)
{
    return IsWindow(window) && IsWindowVisible(window) && !IsIconic(window);
}

// What is captured of a window whose inside measures `client`, before any scaling.
QSize windowPictureSize(const QSize &client)
{
    return evenSize(client).expandedTo(SMALLEST_WINDOW_PICTURE);
}

// Whether a BGRA picture on the card is transparent wherever it is looked at, which no picture
// of a window that has drawn itself is. `scratch` is what it is read back into.
// Into memory in the format the card has it in, into what `memory` holds already when that fits.
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

// The bitmap from its top left corner, with black where it does not reach.
// Whether `bgra`, which copyTopLeft filled from a bitmap of this size, still shows what the
// bitmap does.
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

// Graphics capture, read on a thread of its own. Reading it does not come back until the
// source shows something new, which a minimised window never does, and ffmpeg has no way to
// call a read off. So a capture that is no longer wanted is only abandoned: its thread frees
// it when the read it is in returns, which is when the window is next shown or is closed.
class GraphicsCaptureSession
{
public:
    enum class Taken {
        Picture,
        Nothing,
        Ended,
    };

    // Null when the capture could not be made, with ffmpeg's reason in `error`.
    static std::shared_ptr<GraphicsCaptureSession> start(const QByteArray &chain, const QByteArray &adapter, int *error)
    {
        AVBufferRef *device = nullptr;
        *error = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, adapter.isEmpty() ? nullptr : adapter.constData(), nullptr, 0);
        if (*error < 0)
            return nullptr;

        auto session = std::make_shared<GraphicsCaptureSession>();
        *error = assemble(chain, device, &session->graph, &session->sink);
        av_buffer_unref(&device);
        if (*error < 0)
            return nullptr;
        std::thread([session] { session->run(); }).detach();
        return session;
    }

    ~GraphicsCaptureSession() { avfilter_graph_free(&graph); }

    void abandon()
    {
        const std::lock_guard lock(mutex);
        abandoned = true;
    }

    // What ffmpeg ended the capture with, or zero while it runs: the end of the stream when
    // the window was closed or hidden, an error otherwise.
    [[nodiscard]] int endedWith()
    {
        const std::lock_guard lock(mutex);
        return ended;
    }

    // The newest picture not taken yet, once it is `notBefore`. Waits for one until `giveUpAt`.
    Taken take(AVFrame *into, Clock::time_point notBefore, Clock::time_point giveUpAt)
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (ended)
                return Taken::Ended;
            const auto now = Clock::now();
            if (fresh && now >= notBefore) {
                av_frame_unref(into);
                av_frame_move_ref(into, latest.get());
                fresh = false;
                return Taken::Picture;
            }
            if (now >= giveUpAt)
                return Taken::Nothing;
            arrived.wait_until(lock, fresh ? notBefore : giveUpAt);
        }
    }

private:
    // Windows 10 hands over a picture for every refresh of the screen whatever rate it was
    // asked for, and keeps only two waiting. All are read, so that the one taken is the newest.
    void run()
    {
        const FramePointer picture(av_frame_alloc());
        for (;;) {
            const int got = av_buffersink_get_frame(sink, picture.get());
            const std::lock_guard lock(mutex);
            if (abandoned)
                break;
            if (got < 0) {
                ended = got;
                break;
            }
            av_frame_unref(latest.get());
            av_frame_move_ref(latest.get(), picture.get());
            fresh = true;
            arrived.notify_all();
        }
        arrived.notify_all();
        avfilter_graph_free(&graph);
    }

    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    std::mutex mutex;
    std::condition_variable arrived;
    FramePointer latest{ av_frame_alloc() };
    bool fresh = false;
    bool abandoned = false;
    int ended = 0;
};

// Capturing a window by its handle came with Windows 10 1903 and the filter with ffmpeg 8.1.
// Making the filter loads everything it needs of Windows, and captures nothing.
bool graphicsCaptureUsable()
{
    static const bool initialises = [] {
        const AVFilter *filter = avfilter_get_by_name("gfxcapture");
        if (!filter || QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
            return false;
        AVFilterGraph *graph = avfilter_graph_alloc();
        AVFilterContext *made = nullptr;
        const bool initialised = avfilter_graph_create_filter(&made, filter, "probe", nullptr, nullptr, graph) >= 0;
        avfilter_graph_free(&graph);
        return initialised;
    }();
    return initialises && !graphicsCaptureRuledOut;
}

bool usesGraphicsCapture(CaptureMethod method, bool forWindow)
{
    const bool preferred = method == CaptureMethod::GraphicsCapture || (method == CaptureMethod::Auto && forWindow);
    return graphicsCaptureUsable() && (preferred || !screenCaptureAvailable());
}

std::chrono::microseconds pictureInterval(int framerate)
{
    return std::chrono::microseconds(1'000'000 / std::max(1, framerate));
}

// True, after waiting a little of it, while there is time left until `retryAt`.
bool waitedForRetry(Clock::time_point retryAt)
{
    const auto now = Clock::now();
    if (now >= retryAt)
        return false;
    std::this_thread::sleep_for(std::min<Clock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
    return true;
}

// Frames on `device` for BGRA pictures of one size. Null when the card will not make them.
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

// ddagrab hands the picture it handed over last over again while the screen shows nothing
// new. `last` is a reference to that one, which keeps its place on the card from being given
// to another.
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

// Where `part` of a picture lies once the picture is scaled from one size to another.
QRect scaledPart(const QRect &part, const QSize &from, const QSize &to)
{
    const QPoint corner(part.left() * to.width() / from.width(), part.top() * to.height() / from.height());
    const QPoint opposite((part.left() + part.width()) * to.width() / from.width(), (part.top() + part.height()) * to.height() / from.height());
    return QRect(corner, QSize(opposite.x() - corner.x(), opposite.y() - corner.y()));
}

// The description the capture filters give their pictures is that of the screen's own RGB.
void markColours(AVFrame *picture, bool bt601)
{
    picture->colorspace = bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    picture->color_range = AVCOL_RANGE_MPEG;
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

// `area` of one picture on the graphics card as a picture of its own from `pool`: at `at` of
// it, with black wherever the area does not reach.
bool cutOntoCard(const AVFrame *from, const QRect &area, AVBufferRef *pool, const QPoint &at, AVFrame *to)
{
    if (av_hwframe_get_buffer(pool, to, 0) < 0)
        return false;
    if (area.size() != QSize(to->width, to->height))
        clearToBlack(to);
    copyOnCard(from, area, to, at);
    return true;
}

} // namespace

// The part of its screen's picture that shows a window, and where in the window's own picture
// that part belongs. They differ in size only where the window reaches past its screen.
struct ScreenCapture::CutOut
{
    QRect onScreen;
    QRect inPicture;

    bool operator==(const CutOut &other) const = default;
};

enum class ScreenCapture::Drawing {
    Failed,
    // What the window drew is what the last picture, which it also drew, shows.
    Unchanged,
    Drawn,
};

struct ScreenCapture::Backends
{
    ~Backends()
    {
        av_buffer_unref(&pool);
        av_buffer_unref(&device);
    }

    HWND window = nullptr;
    QSize clientSize;
    Clock::time_point sizeChangedAt;
    // What is captured of a window, before any scaling. It follows the size of the window's
    // inside once that has held.
    QSize pictureSize;
    Clock::time_point pictureDue;

    std::shared_ptr<GraphicsCaptureSession> session;
    // Not before this is a capture made again that ended by itself.
    Clock::time_point remakeAt;
    // Until then, or until one comes that has something in it, a picture of the window may
    // have nothing in it: the window was hidden a moment ago, or its capture has just begun.
    Clock::time_point emptyPicturesUntil;

    // For a window by duplication: the screen it is on, the card that screen is on, and the
    // pictures of the window on that card, which both ways of capturing it fill.
    HMONITOR screenHandle = nullptr;
    std::optional<Monitor> screen;
    bool screenChanged = true;
    AVBufferRef *device = nullptr;
    int deviceAdapter = -1;
    AVBufferRef *pool = nullptr;
    std::shared_ptr<WindowDrawer> drawer;
    quint64 cutOutPictures = 0;
    quint64 drawnPictures = 0;
    std::optional<bool> lastWasCutOut;
    // Why the last picture could not be cut out of the screen, for the log.
    const char *drawnBecause = "";

    PictureConverter converter;
    PictureConverter rescaler;
    CardScaler scaler;
    FramePointer captured{ av_frame_alloc() };
    FramePointer wholeScreen{ av_frame_alloc() };
    FramePointer bgra{ av_frame_alloc() };
    FramePointer asDrawn{ av_frame_alloc() };

    // How a screen's pictures last went on from the card, for the log.
    const char *wentOn = "";
    // `video/skip_unchanged_pictures`, for turning off what follows: a picture that duplication
    // or the window itself gives again unchanged is not delivered again.
    bool skipsUnchanged = true;
    // The screen's picture the last picture delivered was made of, and for a window where
    // in it the window was.
    FramePointer lastOfScreen{ av_frame_alloc() };
    CutOut lastCut;
    // The size of the bitmap `bgra` was filled from, while the last picture delivered is that
    // one. Not valid otherwise.
    QSize drawingDelivered;
    QSize drawingInBgra;
};

bool screenCaptureAvailable()
{
    // Desktop duplication came with Windows 8.
    static const bool available = avfilter_get_by_name("ddagrab") && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8;
    return available;
}

bool windowCaptureAvailable()
{
    return graphicsCaptureUsable() || screenCaptureAvailable();
}

bool captureMethodUsable(CaptureMethod method)
{
    switch (method) {
    case CaptureMethod::GraphicsCapture:
        return graphicsCaptureUsable();
    case CaptureMethod::DesktopDuplication:
        return screenCaptureAvailable();
    case CaptureMethod::Portal:
    case CaptureMethod::X11:
        return false;
    case CaptureMethod::Auto:
        break;
    }
    return screenCaptureAvailable() || graphicsCaptureUsable();
}

QList<CaptureSource> captureSources(CaptureMethod)
{
    QList<CaptureSource> sources;
    if (!screenCaptureAvailable() && !graphicsCaptureUsable())
        return sources;

    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = SCREEN_ID_PREFIX + QString::number(i);
        source.name = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "Screen %1").arg(i + 1);
        source.geometry = monitors[i].geometry;
        sources.append(source);
    }
    return sources;
}

QList<CaptureSource> captureWindows(CaptureMethod)
{
    QList<CaptureSource> windows;
    if (!windowCaptureAvailable())
        return windows;

    const UnscaledCoordinates unscaled;
    EnumWindows(listShareable, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

QImage captureThumbnail(const CaptureSource &source, const QSize &bound)
{
    if (bound.isEmpty())
        return {};

    const UnscaledCoordinates unscaled;
    if (source.kind == CaptureSource::Kind::Window)
        return windowThumbnail(windowOf(source.id), bound);
    if (source.geometry.isEmpty())
        return {};

    const HDC screen = GetDC(nullptr);
    const QImage thumbnail = shrunk(screen, source.geometry, bound);
    ReleaseDC(nullptr, screen);
    return thumbnail;
}

void restoreIfMinimised(const CaptureSource &window)
{
    const HWND handle = windowOf(window.id);
    if (handle && IsIconic(handle))
        ShowWindowAsync(handle, SW_RESTORE);
}

QSize encodeSizeOf(const CaptureSource &source, int maxHeight)
{
    if (source.kind == CaptureSource::Kind::Screen)
        return source.geometry.isEmpty() ? QSize() : encodeSizeFor(evenSize(source.geometry.size()), maxHeight);

    const UnscaledCoordinates unscaled;
    const HWND window = windowOf(source.id);
    const QSize inside = window && showsItself(window) ? clientOnScreen(window).size() : QSize();
    return inside.isEmpty() ? QSize() : encodeSizeFor(windowPictureSize(inside), maxHeight);
}

ScreenCapture::ScreenCapture() : backends(std::make_unique<Backends>()) {}

ScreenCapture::~ScreenCapture()
{
    tearDown();
}

void ScreenCapture::configure(const Request &wanted)
{
    tearDown();
    request = wanted;
    failingSince.reset();
    retryAt = {};
    closed = false;
    failed = false;
    cardScalingFailed = false;
    failureText.clear();

    backends = std::make_unique<Backends>();
    backends->window = windowOf(request.sourceId);
    backends->skipsUnchanged = QSettings().value(QStringLiteral("video/skip_unchanged_pictures"), true).toBool();
    if (usesGraphicsCapture(request.method, backends->window != nullptr))
        path = Path::GraphicsCapture;
    else
        path = backends->window ? Path::WindowByDuplication : Path::Duplication;
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return true;
}

QString ScreenCapture::captureName() const
{
    switch (path) {
    case Path::GraphicsCapture:
        return QStringLiteral("gfxcapture");
    case Path::WindowByDuplication:
        return QStringLiteral("ddagrab %1 + PrintWindow %2 frames").arg(backends->cutOutPictures).arg(backends->drawnPictures);
    case Path::Duplication:
        break;
    }
    return QStringLiteral("ddagrab");
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
    if (failed)
        return Status::Failed;
    if (closed)
        return Status::Closed;

    const UnscaledCoordinates unscaled;
    if (backends->window) {
        const Status watched = watchWindow();
        if (watched != Status::Frame)
            return watched;
    }
    // Only duplication of a screen has to be read to stay in time with it.
    if (!picturesWanted && path != Path::Duplication) {
        std::this_thread::sleep_for(pictureInterval(request.framerate));
        return Status::NoFrame;
    }
    switch (path) {
    case Path::GraphicsCapture:
        return readGraphicsCapture(into);
    case Path::WindowByDuplication:
        return readWindowByDuplication(into);
    case Path::Duplication:
        break;
    }
    return readDuplication(into);
}

// Whether the window is still there and showing, and how large the picture of it is to be.
// Status::Frame when there is a window to read.
ScreenCapture::Status ScreenCapture::watchWindow()
{
    Backends &state = *backends;
    if (!IsWindow(state.window)) {
        qCInfo(LogVideo) << "The window being captured was closed";
        tearDown();
        closed = true;
        return Status::Closed;
    }

    // Graphics capture leaves out by itself a window that has asked to be kept out of
    // captures. Duplication would show it black, and it is not asked to draw itself either.
    DWORD affinity = 0;
    const bool keptOutOfCaptures = path == Path::WindowByDuplication && GetWindowDisplayAffinity(state.window, &affinity) && affinity != 0;
    const QRect client = IsIconic(state.window) || !IsWindowVisible(state.window) || keptOutOfCaptures ? QRect() : clientOnScreen(state.window);
    if (client.isEmpty()) {
        // Hiding a window ends its graphics capture, and minimising it does not.
        if (state.session && state.session->endedWith() != 0)
            state.session.reset();
        dropDuplication();
        state.clientSize = QSize();
        state.emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
        failingSince.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(STALLED_WAIT_MS));
        return Status::Hidden;
    }

    const auto now = Clock::now();
    if (client.size() != state.clientSize) {
        state.clientSize = client.size();
        state.sizeChangedAt = now;
    }
    const QSize wanted = windowPictureSize(client.size());
    const bool held = now - state.sizeChangedAt >= std::chrono::milliseconds(RESIZE_SETTLE_MS);
    if (wanted != state.pictureSize && (state.pictureSize.isEmpty() || held)) {
        state.pictureSize = wanted;
        if (state.session)
            state.session->abandon();
        state.session.reset();
        av_buffer_unref(&state.pool);
        av_frame_unref(state.lastOfScreen.get());
        state.drawingDelivered = QSize();
    }
    return Status::Frame;
}

void ScreenCapture::delivered()
{
    everDelivered = true;
    failingSince.reset();
}

// Whether pictures that have to be scaled, or converted for an encoder that takes none of the
// screen's own, stay on the card for it.
bool ScreenCapture::scalesOnCard() const
{
    return request.keepOnCard && !cardScalingFailed && CardScaler::usable();
}

// `area` of a picture on the card made `placed` of the one that is sent, which stays on the
// card. False when the card would not, after which this capture scales in software.
bool ScreenCapture::scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into)
{
    const CardScaler::Output output = { size, request.bt601Only || !Media::usesBt709AtHeight(size.height()), request.rgbOnCard };
    if (backends->scaler.scale(from, area, output, placed, into))
        return true;
    cardScalingFailed = true;
    return false;
}

ScreenCapture::Status ScreenCapture::readDuplication(AVFrame *into)
{
    if (!graph) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        const int built = buildDuplication();
        if (built < 0)
            return afterFailure(built);
    }

    // Read whether or not a picture is wanted: ddagrab keeps the time between pictures, and
    // makes up for a read that did not come by handing the next ones over at once.
    Backends &state = *backends;
    AVFrame *captured = state.captured.get();
    const int got = av_buffersink_get_frame(sink, captured);
    if (got == AVERROR(EAGAIN))
        return Status::NoFrame;
    if (got < 0)
        return afterFailure(got);

    if (!picturesWanted || (state.skipsUnchanged && isSamePictureOnCard(state.lastOfScreen.get(), captured))) {
        av_frame_unref(captured);
        return Status::NoFrame;
    }
    keepReferenceTo(state.lastOfScreen.get(), captured);
    return deliverFromCard(captured, into);
}

ScreenCapture::Status ScreenCapture::readGraphicsCapture(AVFrame *into)
{
    Backends &state = *backends;
    if (!state.session) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        if (waitedForRetry(state.remakeAt))
            return Status::NoFrame;
        const int built = buildGraphicsCapture();
        if (built < 0 && path != Path::GraphicsCapture)
            return Status::Stalled;
        if (built < 0)
            return afterFailure(built);
    }

    const auto interval = pictureInterval(request.framerate);
    const auto giveUpAt = std::max(Clock::now(), state.pictureDue) + std::chrono::milliseconds(NOTHING_NEW_WAIT_MS);
    // A picture that comes a little before one is due is the one a source at twice the rate
    // has ready, and the next would be late.
    const GraphicsCaptureSession::Taken taken = state.session->take(state.captured.get(), state.pictureDue - interval / 4, giveUpAt);
    if (taken == GraphicsCaptureSession::Taken::Ended) {
        const int ended = state.session->endedWith();
        if (ended != AVERROR_EOF || !state.window)
            return afterFailure(ended);
        // Windows ends the capture of a window that is hidden or closed. Neither is a failure,
        // and which it was the next look at the window tells.
        state.session.reset();
        state.remakeAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        return Status::NoFrame;
    }
    if (taken == GraphicsCaptureSession::Taken::Nothing)
        return Status::NoFrame;
    state.pictureDue = std::max(state.pictureDue + interval, Clock::now() + interval / 2);
    // Graphics capture hands over a picture with nothing in it, transparent all over, as a
    // window is hidden or closed, and another as it is shown, before the window has drawn
    // itself. Sent on, it is black for as long as the window stays away. Those of before it
    // was hidden wait in the capture until it shows again, so how long after is no measure:
    // they are looked for until the first picture that has something in it.
    bool undrawn = state.window && !showsItself(state.window);
    if (state.window && !undrawn && Clock::now() < state.emptyPicturesUntil) {
        undrawn = isEmptyPicture(state.captured.get(), state.bgra.get());
        if (!undrawn) {
            state.emptyPicturesUntil = {};
            av_frame_unref(state.bgra.get());
        }
    }
    if (undrawn) {
        av_frame_unref(state.captured.get());
        return Status::NoFrame;
    }

    return deliverFromCard(state.captured.get(), into);
}

// A picture on the card made what the encoder is to have: handed over as it is, scaled on the
// card, or read back and scaled in software. `captured` is left empty.
ScreenCapture::Status ScreenCapture::deliverFromCard(AVFrame *captured, AVFrame *into)
{
    Backends &state = *backends;
    const QSize ownSize(captured->width, captured->height);
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const char *wentOn = "read back and scaled in software";
    const bool bt601 = request.bt601Only || !Media::usesBt709AtHeight(size.height());
    if (request.keepOnCard && request.rgbOnCard && size == ownSize) {
        wentOn = "kept on the graphics card as it is";
        av_frame_move_ref(into, captured);
        markColours(into, false);
    } else if (scalesOnCard() && scaleOnCard(captured, wholeOf(captured), size, QRect(QPoint(0, 0), size), into)) {
        wentOn = "scaled on the graphics card and kept there";
        av_frame_unref(captured);
    } else {
        int converted = readBack(captured, state.bgra.get());
        av_frame_unref(captured);
        if (converted >= 0)
            converted = state.converter.convert(state.bgra.get(), size, request.softwareFormat, bt601, into);
        if (converted < 0)
            return afterFailure(converted);
        markColours(into, bt601);
    }

    if (path == Path::Duplication && std::exchange(state.wentOn, wentOn) != wentOn)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << wentOn;
    delivered();
    return Status::Frame;
}

// The official client's way with a window on Windows 10. Every picture is first looked for on
// the screen, and where the screen cannot give the window by itself the window draws that
// picture. Either way it ends up the same size and in the same place, so the encoder never
// knows which it was.
ScreenCapture::Status ScreenCapture::readWindowByDuplication(AVFrame *into)
{
    Backends &state = *backends;
    std::this_thread::sleep_until(state.pictureDue);
    state.pictureDue = std::max(state.pictureDue + pictureInterval(request.framerate), Clock::now());

    const QRect client = clientOnScreen(state.window);
    const bool screenDuplicated = duplicateScreenOf(client);
    const QSize size = encodeSizeFor(state.pictureSize, request.maxHeight);
    const bool asCaptured = state.pool && request.keepOnCard && request.rgbOnCard && size == state.pictureSize;
    const bool scaledOnCard = state.pool && !asCaptured && scalesOnCard();
    const bool staysOnCard = asCaptured || scaledOnCard;

    AVFrame *whole = state.wholeScreen.get();
    AVFrame *card = state.captured.get();
    AVFrame *bgra = state.bgra.get();
    CutOut cut;
    state.drawnBecause = "its screen cannot be duplicated";
    const bool cutOut = screenDuplicated && cutOutOfScreen(client, &cut);
    if (cutOut && state.skipsUnchanged && isSamePictureOnCard(state.lastOfScreen.get(), whole) && cut == state.lastCut) {
        av_frame_unref(whole);
        return Status::NoFrame;
    }
    if (!cutOut && drawnByItself(client) != Drawing::Drawn)
        return Status::NoFrame;

    bool ready = true;
    if (cutOut && scaledOnCard) {
        ready = scaleOnCard(whole, cut.onScreen, size, scaledPart(cut.inPicture, state.pictureSize, size), into);
    } else {
        // The window's own picture first: on the card if that is where it stays, in memory if not.
        if (cutOut)
            ready = cutOntoCard(whole, cut.onScreen, state.pool, cut.inPicture.topLeft(), card) && (staysOnCard || (prepareBgra(bgra, state.pictureSize) && av_hwframe_transfer_data(bgra, card, 0) >= 0));
        else if (staysOnCard)
            ready = av_hwframe_get_buffer(state.pool, card, 0) >= 0 && av_hwframe_transfer_data(card, bgra, 0) >= 0;

        const bool bt601 = !staysOnCard && (request.bt601Only || !Media::usesBt709AtHeight(size.height()));
        if (ready && scaledOnCard)
            ready = scaleOnCard(card, wholeOf(card), size, QRect(QPoint(0, 0), size), into);
        else if (ready && staysOnCard)
            av_frame_move_ref(into, card);
        else if (ready)
            ready = state.converter.convert(bgra, size, request.softwareFormat, bt601, into) >= 0;
        if (ready && !scaledOnCard)
            markColours(into, bt601);
    }
    av_frame_unref(card);
    av_frame_unref(state.lastOfScreen.get());
    if (ready && cutOut) {
        av_frame_move_ref(state.lastOfScreen.get(), whole);
        state.lastCut = cut;
    }
    state.drawingDelivered = ready && !cutOut ? state.drawingInBgra : QSize();
    av_frame_unref(whole);
    if (!ready)
        return Status::NoFrame;

    if (state.lastWasCutOut != cutOut) {
        state.lastWasCutOut = cutOut;
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << (staysOnCard ? "on the graphics card," : "scaled in software,") << (cutOut ? "cut out of its screen" : "drawn by itself, as") << (cutOut ? "" : state.drawnBecause);
    }
    (cutOut ? state.cutOutPictures : state.drawnPictures)++;
    delivered();
    return Status::Frame;
}

// Sees to it that the screen under the middle of the window is being duplicated, and that
// there is a card to keep the window's pictures on. False when that screen cannot be read now.
bool ScreenCapture::duplicateScreenOf(const QRect &client)
{
    Backends &state = *backends;
    const QPoint middle = client.center();
    const HMONITOR under = MonitorFromPoint({ middle.x(), middle.y() }, MONITOR_DEFAULTTONULL);
    const bool retryDue = Clock::now() >= retryAt;
    if (under != state.screenHandle || (state.screenChanged && retryDue)) {
        dropDuplication();
        state.screenHandle = under;
        state.screenChanged = false;
        state.screen.reset();
        for (const Monitor &each : attachedMonitors()) {
            if (each.handle == under)
                state.screen = each;
        }
    }

    const int adapter = state.screen ? state.screen->adapter : std::max(0, state.deviceAdapter);
    if (adapter != state.deviceAdapter || (!state.device && retryDue)) {
        dropDuplication();
        av_buffer_unref(&state.pool);
        av_buffer_unref(&state.device);
        state.deviceAdapter = adapter;
        if (av_hwdevice_ctx_create(&state.device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(adapter).constData(), nullptr, 0) < 0)
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    if (state.device && !state.pool)
        state.pool = cardPictures(state.device, state.pictureSize);

    // ddagrab does not turn a turned screen's picture upright.
    if (!state.pool || !state.screen || state.screen->turned)
        return false;
    if (graph)
        return true;
    if (!retryDue)
        return false;

    const QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(state.screen->output) + ":framerate=" + QByteArray::number(request.framerate);
    const int built = assemble(chain, state.device, &graph, &sink);
    if (built < 0) {
        // Until Windows gives the screen back, which it also takes for a mode it may come
        // back in another size with.
        state.screenChanged = true;
        retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    return built >= 0;
}

// Where the window's picture is in the screen's, which is then the picture read last. False
// when the screen does not show the window by itself, or not where it was when the screen was
// read.
bool ScreenCapture::cutOutOfScreen(const QRect &client, CutOut *cut)
{
    Backends &state = *backends;
    // While its size is changing, and where it is smaller than what is sent, the window does
    // not fill the picture.
    state.drawnBecause = "its size is changing, or is smaller than what is sent";
    if (evenSize(client.size()) != state.pictureSize)
        return false;
    state.drawnBecause = "a window above overlaps it";
    if (isCoveredOnScreen(state.window, client, ShellSurfaces::LeftForLater))
        return false;

    state.drawnBecause = "it reaches past its screen";
    const QRect screen = state.screen->geometry;
    if (!screen.adjusted(-REACH_PAST_SCREEN, -REACH_PAST_SCREEN, REACH_PAST_SCREEN, REACH_PAST_SCREEN).contains(client))
        return false;
    const QRect onScreen = client & screen;
    const QRect inPicture = QRect(onScreen.topLeft() - client.topLeft(), onScreen.size()) & QRect(QPoint(0, 0), state.pictureSize);

    state.drawnBecause = "its screen had no picture";
    AVFrame *whole = state.wholeScreen.get();
    av_frame_unref(whole);
    const int got = av_buffersink_get_frame(sink, whole);
    if (got < 0) {
        if (got != AVERROR(EAGAIN)) {
            dropDuplication();
            state.screenChanged = true;
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        }
        return false;
    }

    // The read waits for the screen, and a window moved or covered meanwhile has left its
    // place there to something else.
    state.drawnBecause = "it moved or was covered while its screen was read";
    if (clientOnScreen(state.window) != client || isCoveredOnScreen(state.window, client, ShellSurfaces::LookedFor)) {
        av_frame_unref(whole);
        return false;
    }
    *cut = { inPicture.translated(client.topLeft() - screen.topLeft()), inPicture };
    return true;
}

// The window's picture as the window draws it, into the BGRA picture kept for it.
ScreenCapture::Drawing ScreenCapture::drawnByItself(const QRect &client)
{
    Backends &state = *backends;
    if (!state.drawer)
        state.drawer = WindowDrawer::start(state.window, true);
    if (!state.drawer->draw(std::chrono::milliseconds(DRAWING_LIMIT_MS), stillWanted))
        return Drawing::Failed;
    state.drawer->drawPointer(client);

    const Bitmap &drawn = state.drawer->drawn();
    AVFrame *bgra = state.bgra.get();
    const QSize delivered = std::exchange(state.drawingDelivered, QSize());
    state.drawingInBgra = QSize();
    if (drawn.size() == client.size()) {
        if (!prepareBgra(bgra, state.pictureSize))
            return Drawing::Failed;
        if (state.skipsUnchanged && delivered == drawn.size() && showsTopLeftOf(drawn, bgra)) {
            state.drawingDelivered = delivered;
            return Drawing::Unchanged;
        }
        copyTopLeft(drawn, bgra);
        state.drawingInBgra = drawn.size();
        return Drawing::Drawn;
    }

    // A program Windows scales for the screen draws itself at its own, smaller size.
    AVFrame *asDrawn = state.asDrawn.get();
    if (!prepareBgra(asDrawn, drawn.size()))
        return Drawing::Failed;
    copyTopLeft(drawn, asDrawn);
    av_frame_unref(bgra);
    return state.rescaler.convert(asDrawn, state.pictureSize, AV_PIX_FMT_BGRA, false, bgra) >= 0 ? Drawing::Drawn : Drawing::Failed;
}

// Neither source recovers from losing what it captures: ddagrab not from losing the screen nor
// from a changed screen mode, graphics capture not from its window being hidden. The capture
// has to be made again, and until Windows gives the source back that fails.
ScreenCapture::Status ScreenCapture::afterFailure(int error)
{
    tearDown();

    const auto now = Clock::now();
    // Access is denied for as long as the secure desktop shows, which is as long as the
    // user leaves a UAC prompt open or the machine locked, and no reason to stop sharing.
    const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
    if (!failingSince || secureDesktopShowing)
        failingSince = now;
    if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
        failed = true;
        const QString reason = backends->window ? QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "The window can no longer be captured (%1).") : QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "The screen can no longer be captured (%1).");
        failureText = reason.arg(describeError(error));
        qCWarning(LogVideo) << "Giving up on capturing" << request.sourceId << ":" << describeError(error);
        return Status::Failed;
    }
    retryAt = now + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    return Status::Stalled;
}

// The screen's pictures come out of the duplication on the card whatever is made of them
// after, because that is where one that shows nothing new can be told from the one before it.
int ScreenCapture::buildDuplication()
{
    const std::vector<Monitor> monitors = attachedMonitors();
    const Monitor *monitor = numberedMonitor(monitors, request.sourceId);
    if (!monitor)
        return AVERROR(ENODEV);

    // ddagrab counts outputs on the adapter of the device it is given.
    AVBufferRef *device = nullptr;
    int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(monitor->adapter).constData(), nullptr, 0);
    if (result < 0)
        return result;

    const QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(monitor->output) + ":framerate=" + QByteArray::number(request.framerate);
    result = assemble(chain, device, &graph, &sink);
    av_buffer_unref(&device);
    backends->wentOn = "";
    return result;
}

int ScreenCapture::buildGraphicsCapture()
{
    Backends &state = *backends;

    QSize ownSize = state.pictureSize;
    QByteArray source;
    QByteArray adapter;
    if (state.window) {
        source = ":hwnd=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(state.window)));
    } else {
        const std::vector<Monitor> monitors = attachedMonitors();
        const Monitor *monitor = numberedMonitor(monitors, request.sourceId);
        if (!monitor)
            return AVERROR(ENODEV);
        ownSize = evenSize(monitor->geometry.size());
        source = ":hmonitor=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor->handle)));
        adapter = QByteArray::number(monitor->adapter);
    }

    // Graphics capture scales by itself, into BGRA pictures an encoder has to take as they are.
    // It stretches whatever it has captured over the whole picture, which is right for a source
    // that fills its picture, and a window smaller than the smallest picture does not.
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const bool asCaptured = request.keepOnCard && request.rgbOnCard && size == ownSize;
    const bool fillsItsPicture = !state.window || evenSize(state.clientSize) == ownSize;
    const bool scalesByItself = request.keepOnCard && request.rgbOnCard && size != ownSize && fillsItsPicture;
    const QSize captured = scalesByItself ? size : ownSize;

    // The size is given because the filter otherwise fixes it before the first picture, from
    // what Windows says the window measures, which is not always what it then hands over.
    QByteArray chain = "gfxcapture=max_framerate=" + QByteArray::number(request.framerate) + source + ":width=" + QByteArray::number(captured.width()) + ":height=" + QByteArray::number(captured.height());
    if (scalesByItself)
        chain += ":resize_mode=scale";
    int result = 0;
    state.session = GraphicsCaptureSession::start(chain, adapter, &result);
    if (state.window)
        state.emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
    if (result >= 0) {
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "by graphics capture as" << size.width() << "x" << size.height() << (asCaptured ? "kept on the graphics card as it is" : scalesByItself ? "scaled by graphics capture and kept on the graphics card"
                                                                                                                                                                                : scalesOnCard()       ? "scaled on the graphics card and kept there"
                                                                                                                                                                                                       : "read back and scaled in software");
        return result;
    }

    if (result == AVERROR(ENOSYS))
        graphicsCaptureRuledOut = true;
    // Left to choose, what graphics capture will not take is duplicated.
    const bool leftToChoose = request.method == CaptureMethod::Auto && state.window && !everDelivered;
    if ((result == AVERROR(ENOSYS) || leftToChoose) && screenCaptureAvailable()) {
        qCInfo(LogVideo) << "Graphics capture of" << request.sourceId << "failed (" << describeError(result) << "); using desktop duplication";
        path = state.window ? Path::WindowByDuplication : Path::Duplication;
    }
    return result;
}

void ScreenCapture::dropDuplication()
{
    avfilter_graph_free(&graph);
    sink = nullptr;
    av_frame_unref(backends->lastOfScreen.get());
}

void ScreenCapture::tearDown()
{
    dropDuplication();
    if (backends->session)
        backends->session->abandon();
    backends->session.reset();
    if (backends->drawer)
        backends->drawer->abandon();
    backends->drawer.reset();
}

#elif !defined(ACHERON_LINUX_SCREEN_CAPTURE)

struct ScreenCapture::Backends
{
};

bool screenCaptureAvailable()
{
    return false;
}

bool windowCaptureAvailable()
{
    return false;
}

bool captureMethodUsable(CaptureMethod)
{
    return false;
}

QList<CaptureSource> captureSources(CaptureMethod)
{
    return {};
}

QList<CaptureSource> captureWindows(CaptureMethod)
{
    return {};
}

QImage captureThumbnail(const CaptureSource &, const QSize &)
{
    return {};
}

void restoreIfMinimised(const CaptureSource &)
{
}

QSize encodeSizeOf(const CaptureSource &, int)
{
    return {};
}

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &wanted)
{
    request = wanted;
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return false;
}

QString ScreenCapture::captureName() const
{
    return {};
}

ScreenCapture::Status ScreenCapture::read(AVFrame *)
{
    failureText = QCoreApplication::translate("Acheron::Core::Video::BroadcastPipeline", "This build cannot capture the screen.");
    return Status::Failed;
}

#endif // ACHERON_SCREEN_CAPTURE

} // namespace Video
} // namespace Core
} // namespace Acheron
