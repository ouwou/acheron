#include "AddAccountDialog.hpp"

namespace Acheron {
namespace UI {

AddAccountDialog::AddAccountDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Add Account"));
    resize(400, 150);

    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *instruction = new QLabel(tr("Enter your Discord Authentication Token:"), this);
    layout->addWidget(instruction);

    tokenInput = new QLineEdit(this);
    tokenInput->setEchoMode(QLineEdit::Password);
    layout->addWidget(tokenInput);

    QDialogButtonBox *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString AddAccountDialog::getToken() const
{
    return tokenInput->text().trimmed();
}

} // namespace UI
} // namespace Acheron