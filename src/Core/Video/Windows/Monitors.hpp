#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QRect>
#include <QString>

#include <vector>

#include <windows.h>

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

struct Monitor
{
    // What Windows calls the output, such as \\.\DISPLAY1. It stays the output's while the
    // others come and go, which its place among them does not.
    QString deviceName;
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

// The monitors attached to the desktop, in the order DXGI lists them.
[[nodiscard]] std::vector<Monitor> attachedMonitors();

// "screen:<device name>".
[[nodiscard]] QString screenSourceId(const Monitor &monitor);
// The monitor a screen source is, while it is attached. Null once it is not.
[[nodiscard]] const Monitor *monitorOf(const std::vector<Monitor> &monitors, const QString &sourceId);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
