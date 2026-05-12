#pragma once

#include <QtWidgets>

namespace Acheron {
namespace UI {

class TokenInputDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TokenInputDialog(const QString &title, const QString &prompt, QWidget *parent = nullptr);
    QString getToken() const;

private:
    QLineEdit *tokenInput;
};

} // namespace UI
} // namespace Acheron
