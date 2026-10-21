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

    [[nodiscard]] static DPI_AWARENESS_CONTEXT of(HWND window) { return windowAwareness ? windowAwareness(window) : nullptr; }

private:
    // missing before Windows 10 1607
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using WindowAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(HWND);
    static inline const SetAwareness setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    static inline const WindowAwareness windowAwareness = reinterpret_cast<WindowAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext")));
    DPI_AWARENESS_CONTEXT previous = nullptr;
};

// Windows scales the coordinates it gives a thread that hasnt declared DPI awareness
class UnscaledCoordinates : public ThreadScaling
{
public:
    UnscaledCoordinates() : ThreadScaling(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) {}
};

[[nodiscard]] QRect fromNative(const RECT &rect);
[[nodiscard]] HWND windowOf(const QString &sourceId);
[[nodiscard]] bool isCloaked(HWND window);
// GetWindowRect also counts the invisible borders Windows 10 puts around a resizable window
[[nodiscard]] QRect visibleBounds(HWND window);
[[nodiscard]] QRect clientOnScreen(HWND window);
[[nodiscard]] bool showsItself(HWND window);

enum class ShellSurfaces {
    LookedFor,
    LeftForLater,
};
[[nodiscard]] bool isCoveredOnScreen(HWND window, const QRect &client, ShellSurfaces shellSurfaces);

[[nodiscard]] QList<CaptureSource> shareableWindows();

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
