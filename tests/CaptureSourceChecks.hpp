#pragma once

#include "Core/Video/CaptureSources.hpp"

#include <QList>
#include <QSet>
#include <QString>

namespace CaptureSourceChecks {

// names a window by its id alone: titles stay out of the log
[[nodiscard]] inline QString problemWithListedWindows(const QList<Acheron::Core::Video::CaptureSource> &windows, quint32 ownProcessId)
{
    using Acheron::Core::Video::CaptureSource;
    QSet<QString> ids;
    for (const CaptureSource &window : windows) {
        if (window.kind != CaptureSource::Kind::Window || Acheron::Core::Video::windowHandleOf(window.id).value_or(0) == 0)
            return QStringLiteral("%1 is not listed as a window").arg(window.id);
        if (ids.contains(window.id))
            return QStringLiteral("%1 is listed twice").arg(window.id);
        if (window.name.isEmpty())
            return QStringLiteral("%1 has no title").arg(window.id);
        if (window.processId == ownProcessId)
            return QStringLiteral("%1 is this program's own").arg(window.id);
        ids.insert(window.id);
    }
    return {};
}

} // namespace CaptureSourceChecks
