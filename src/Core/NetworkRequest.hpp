#pragma once

#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QUrl>

namespace Acheron {
namespace Core {

inline QNetworkRequest networkRequest(const QUrl &url)
{
    QNetworkRequest request(url);
#ifdef Q_OS_WIN
    static const bool schannelLacksAlpn = QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows8_1;
    if (schannelLacksAlpn)
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
    return request;
}

} // namespace Core
} // namespace Acheron
