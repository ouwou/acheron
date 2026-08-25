#include "ProxyConfig.hpp"

#include <QUrl>

namespace Acheron {
namespace Core {

namespace {

QString curlScheme(ProxyConfig::Type type)
{
    switch (type) {
    case ProxyConfig::Type::Http:
        return QStringLiteral("http");
    case ProxyConfig::Type::Https:
        return QStringLiteral("https");
    case ProxyConfig::Type::Socks5:
        return QStringLiteral("socks5h");
    case ProxyConfig::Type::None:
        break;
    }
    return QString();
}

QString displayScheme(ProxyConfig::Type type)
{
    if (type == ProxyConfig::Type::Socks5)
        return QStringLiteral("socks5");

    return curlScheme(type);
}

QString authority(const QString &host, quint16 port)
{
    const bool ipv6 = host.contains(QLatin1Char(':'));
    return (ipv6 ? QStringLiteral("[%1]:%2") : QStringLiteral("%1:%2")).arg(host, QString::number(port));
}

QNetworkProxy::ProxyType qtProxyType(ProxyConfig::Type type)
{
    // https proxy degrades to plain CONNECT in qt because it doesnt support it
    if (type == ProxyConfig::Type::Socks5)
        return QNetworkProxy::Socks5Proxy;

    return QNetworkProxy::HttpProxy;
}

} // namespace

QString ProxyConfig::toCurlUrl() const
{
    if (!enabled())
        return QString();

    return QStringLiteral("%1://%2").arg(curlScheme(type), authority(host, port));
}

QNetworkProxy ProxyConfig::toQtProxy() const
{
    if (!enabled())
        return QNetworkProxy(QNetworkProxy::NoProxy);

    QNetworkProxy proxy(qtProxyType(type), host, port);
    if (!username.isEmpty()) {
        proxy.setUser(username);
        proxy.setPassword(password);
    }

    proxy.setCapabilities(proxy.capabilities() | QNetworkProxy::HostNameLookupCapability);

    return proxy;
}

QString ProxyConfig::toString() const
{
    if (!enabled())
        return QString();

    QString credentials;
    if (!username.isEmpty()) {
        credentials = QString::fromUtf8(QUrl::toPercentEncoding(username));
        if (!password.isEmpty())
            credentials += QStringLiteral(":") + QString::fromUtf8(QUrl::toPercentEncoding(password));
        credentials += QStringLiteral("@");
    }

    return QStringLiteral("%1://%2%3")
            .arg(displayScheme(type), credentials, authority(host, port));
}

QString ProxyConfig::toDisplayString() const
{
    if (!enabled())
        return QString();

    return QStringLiteral("%1://%2").arg(displayScheme(type), authority(host, port));
}

std::optional<ProxyConfig> ProxyConfig::parse(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return ProxyConfig{};

    const QUrl url(trimmed);
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;

    ProxyConfig config;

    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http"))
        config.type = Type::Http;
    else if (scheme == QStringLiteral("https"))
        config.type = Type::Https;
    else if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks5h") || scheme == QStringLiteral("socks"))
        config.type = Type::Socks5;
    else
        return std::nullopt;

    const int port = url.port();
    if (port <= 0 || port > 65535)
        return std::nullopt;

    config.host = url.host();
    config.port = static_cast<quint16>(port);
    config.username = url.userName();
    config.password = url.password();

    return config;
}

} // namespace Core
} // namespace Acheron
