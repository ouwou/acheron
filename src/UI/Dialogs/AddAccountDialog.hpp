#pragma once

#include <QtWidgets>

namespace Acheron {
namespace UI {

class AddAccountDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddAccountDialog(QWidget *parent = nullptr);
    QString getToken() const;

private:
    QLineEdit *tokenInput;
};

} // namespace UI
} // namespace Acheron
