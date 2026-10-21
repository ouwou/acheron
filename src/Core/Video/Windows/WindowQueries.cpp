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

// missing from headers that also target Windows 7
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
    const std::optional<quint64> handle = windowHandleOf(sourceId);
    return handle ? reinterpret_cast<HWND>(static_cast<quintptr>(*handle)) : nullptr;
}

// as the official client counts it. a Windows without cloaks calls the argument invalid
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
        // a foreground window has come forward, whatever its styles say
        each.overlay = (extended & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0 && window != stack->foreground;
        each.tabProxy = hasClass(window, L"Windows.Internal.Shell.TabProxyWindow");
        each.livePreview = (extended & WS_EX_TOPMOST) && hasClass(window, L"LivePreview");
    }
    stack->topDown.append(each);
    return TRUE;
}

// Task View, Start and the like are shell windows above every ordinary one, cloaked until shown, which
// FindWindowEx finds and EnumWindows doesnt. the official client never sees them
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

// a store app's window belongs to a host process; the app is the process of the window inside it
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
    source.id = windowSourceId(static_cast<quint64>(reinterpret_cast<quintptr>(window)));
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
