#include "TokenInputDialog.hpp"

namespace Acheron {
namespace UI {

TokenInputDialog::TokenInputDialog(const QString &title, const QString &prompt, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    resize(400, 150);

    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *instruction = new QLabel(prompt, this);
    instruction->setWordWrap(true);
    layout->addWidget(instruction);

    tokenInput = new QLineEdit(this);
    tokenInput->setEchoMode(QLineEdit::Password);
    layout->addWidget(tokenInput);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString TokenInputDialog::getToken() const
{
    return tokenInput->text().trimmed();
}

} // namespace UI
} // namespace Acheron
