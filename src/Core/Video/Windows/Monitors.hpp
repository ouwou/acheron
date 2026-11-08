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
    QString deviceName;
    int adapter = 0;
    // as ddagrab counts them: attached or not
    int output = 0;
    HMONITOR handle = nullptr;
    QRect geometry;
    bool turned = false;
};

[[nodiscard]] std::vector<Monitor> attachedMonitors();

[[nodiscard]] QString screenSourceId(const Monitor &monitor);
[[nodiscard]] const Monitor *monitorOf(const std::vector<Monitor> &monitors, const QString &sourceId);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
