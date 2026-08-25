#pragma once

#include <QtWidgets>

#include "Core/ProxyConfig.hpp"
#include "UI/ProxyLineEdit.hpp"

namespace Acheron {
namespace UI {

class TokenInputDialog : public QDialog
{
    Q_OBJECT
public:
    enum class ProxyField {
        Hidden,
        Shown,
    };

    explicit TokenInputDialog(const QString &title,
                              const QString &prompt,
                              QWidget *parent = nullptr,
                              ProxyField proxyField = ProxyField::Hidden);
    QString getToken() const;
    [[nodiscard]] Core::ProxyConfig getProxy() const { return proxy; }

private:
    void accept() override;

    QLineEdit *tokenInput;
    ProxyLineEdit *proxyInput = nullptr;
    Core::ProxyConfig proxy;
};

} // namespace UI
} // namespace Acheron
