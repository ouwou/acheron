#include "ProxyLineEdit.hpp"

#include <QMessageBox>

namespace Acheron {
namespace UI {

ProxyLineEdit::ProxyLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
    setPlaceholderText(tr("No proxy - e.g. socks5://user:pass@host:1080"));
}

std::optional<Core::ProxyConfig> ProxyLineEdit::parseOrWarn()
{
    std::optional<Core::ProxyConfig> proxy = Core::ProxyConfig::parse(text());
    if (!proxy)
        QMessageBox::warning(this,
                             tr("Invalid proxy"),
                             tr("Enter a proxy as <b>scheme://[user:password@]host:port</b>, "
                                "where scheme is http, https or socks5."));

    return proxy;
}

} // namespace UI
} // namespace Acheron
