#pragma once

#include <QtWidgets>

#include "Core/ProxyConfig.hpp"
#include "UI/ProxyLineEdit.hpp"

namespace Acheron {
namespace UI {

class ProxyInputDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProxyInputDialog(const QString &title, const QString &prompt, QWidget *parent = nullptr);

    [[nodiscard]] Core::ProxyConfig getProxy() const { return proxy; }

private:
    void accept() override;

    ProxyLineEdit *proxyInput;
    Core::ProxyConfig proxy;
};

} // namespace UI
} // namespace Acheron
