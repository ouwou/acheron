#pragma once

#include <QLibraryInfo>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QUrl>
#include <QVersionNumber>

namespace Acheron {
namespace Core {

// QTBUG-147237: an HTTP/2 connection error leaves the dead connection cached, and every later request to that host hangs
[[nodiscard]] inline bool qtKeepsDeadHttp2Connections()
{
    static const QVersionNumber firstAffected(6, 8, 0);
    static const QVersionNumber firstFixed(6, 12, 0);
    static const bool affected = QLibraryInfo::version() >= firstAffected && QLibraryInfo::version() < firstFixed;
    return affected;
}

[[nodiscard]] inline bool schannelLacksAlpn()
{
#ifdef Q_OS_WIN
    static const bool lacking = QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows8_1;
    return lacking;
#else
    return false;
#endif
}

inline QNetworkRequest networkRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    if (schannelLacksAlpn() || qtKeepsDeadHttp2Connections())
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    return request;
}

} // namespace Core
} // namespace Acheron
