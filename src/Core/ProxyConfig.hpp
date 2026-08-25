#pragma once

#include <QNetworkProxy>
#include <QString>

#include <optional>

namespace Acheron {
namespace Core {

struct ProxyConfig
{
    enum class Type {
        None,
        Http,
        Https,
        Socks5
    };

    Type type = Type::None;
    QString host;
    quint16 port = 0;
    QString username;
    QString password;

    [[nodiscard]] bool enabled() const noexcept { return type != Type::None && !host.isEmpty() && port != 0; }
    [[nodiscard]] bool canRelayUdp() const noexcept { return !enabled() || type == Type::Socks5; }

    [[nodiscard]] QString toCurlUrl() const;
    [[nodiscard]] QNetworkProxy toQtProxy() const;
    [[nodiscard]] QString toString() const;
    [[nodiscard]] QString toDisplayString() const;

    static std::optional<ProxyConfig> parse(const QString &text);

    bool operator==(const ProxyConfig &) const = default;
};

} // namespace Core
} // namespace Acheron
