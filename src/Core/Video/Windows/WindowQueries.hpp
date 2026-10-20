#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QList>
#include <QRect>
#include <QString>

#include <windows.h>

#include "Core/Video/CaptureSources.hpp"

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

// What Windows says of its windows, in the terms the capture asks it.

inline const QLatin1String WINDOW_ID_PREFIX("window:");

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

[[nodiscard]] QRect fromNative(const RECT &rect);
// The window a "window:<handle>" source is, or null.
[[nodiscard]] HWND windowOf(const QString &sourceId);
[[nodiscard]] bool isCloaked(HWND window);
// The window as it shows on the screen. GetWindowRect also counts the invisible borders
// Windows 10 puts around a window that can be resized.
[[nodiscard]] QRect visibleBounds(HWND window);
// The inside of the window's frame, which is what is shared of it.
[[nodiscard]] QRect clientOnScreen(HWND window);
[[nodiscard]] bool showsItself(HWND window);

enum class ShellSurfaces {
    LookedFor,
    // For a first look that a second, after the screen is read, decides over.
    LeftForLater,
};
// Whether the window cannot be cut out of the screen by itself: windowIsCovered() of the
// windows above it as they are now.
[[nodiscard]] bool isCoveredOnScreen(HWND window, const QRect &client, ShellSurfaces shellSurfaces);

// The windows a user would call ones that can be shared.
[[nodiscard]] QList<CaptureSource> shareableWindows();

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
