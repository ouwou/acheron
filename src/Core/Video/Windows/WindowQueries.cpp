#include "Core/Video/Windows/WindowQueries.hpp"

#if defined(Q_OS_WIN)

#include "Core/Video/ScreenCapture.hpp"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

// DwmGetWindowAttribute's DWMWA_EXTENDED_FRAME_BOUNDS and DWMWA_CLOAKED, which the headers keep
// from a build that also targets Windows 7.
constexpr DWORD FRAME_BOUNDS_ATTRIBUTE = 9;
constexpr DWORD CLOAKED_ATTRIBUTE = 14;

HRESULT windowAttribute(HWND window, DWORD attribute, void *value, DWORD size)
{
    using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = reinterpret_cast<GetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
    return getAttribute ? getAttribute(window, attribute, value, size) : E_INVALIDARG;
}

bool hasClass(HWND window, const wchar_t *name)
{
    wchar_t found[64] = {};
    return GetClassNameW(window, found, 64) > 0 && wcscmp(found, name) == 0;
}

} // namespace

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

// Kept from the screen by the program (1) or by the shell (2), as the official client counts
// it. A Windows that does not know the question calls the argument invalid, and has no cloaks.
bool isCloaked(HWND window)
{
    DWORD cloak = 0;
    const HRESULT asked = windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak));
    return SUCCEEDED(asked) ? cloak == 1 || cloak == 2 : asked != E_INVALIDARG;
}

QRect visibleBounds(HWND window)
{
    RECT bounds = {};
    if (FAILED(windowAttribute(window, FRAME_BOUNDS_ATTRIBUTE, &bounds, sizeof(bounds))) || IsRectEmpty(&bounds)) {
        if (!GetWindowRect(window, &bounds))
            return {};
    }
    return fromNative(bounds);
}

QRect clientOnScreen(HWND window)
{
    RECT client = {};
    POINT corner = {};
    if (GetClientRect(window, &client) && ClientToScreen(window, &corner))
        return QRect(corner.x, corner.y, client.right, client.bottom);
    return GetWindowRect(window, &client) ? fromNative(client) : QRect();
}

namespace {

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

} // namespace

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

QList<CaptureSource> shareableWindows()
{
    QList<CaptureSource> windows;
    const UnscaledCoordinates unscaled;
    EnumWindows(listShareable, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

bool showsItself(HWND window)
{
    return IsWindow(window) && IsWindowVisible(window) && !IsIconic(window);
}

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
