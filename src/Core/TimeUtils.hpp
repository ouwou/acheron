#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

namespace Acheron {
namespace Core {
namespace TimeUtils {

static QString relativeTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return {};
    qint64 secs = (QDateTime::currentMSecsSinceEpoch() - dt.toMSecsSinceEpoch()) / 1000;
    if (secs < 0)
        secs = 0;
    if (secs < 60)
        return QStringLiteral("just now");
    if (secs < 3600)
        return QString::number(secs / 60) + QStringLiteral("m ago");
    if (secs < 86400)
        return QString::number(secs / 3600) + QStringLiteral("h ago");
    if (secs < 2592000)
        return QString::number(secs / 86400) + QStringLiteral("d ago");
    return dt.toLocalTime().toString(QStringLiteral("MMM d"));
}

static QString absoluteTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return {};
    return QLocale::system().toString(dt.toLocalTime(), QLocale::ShortFormat);
}

} // namespace TimeUtils
} // namespace Core
} // namespace Acheron
