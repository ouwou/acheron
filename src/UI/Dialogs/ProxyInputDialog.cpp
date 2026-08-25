#include "ProxyInputDialog.hpp"

namespace Acheron {
namespace UI {

ProxyInputDialog::ProxyInputDialog(const QString &title, const QString &prompt, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    resize(400, 140);

    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *instruction = new QLabel(prompt, this);
    instruction->setWordWrap(true);
    layout->addWidget(instruction);

    proxyInput = new ProxyLineEdit(this);
    layout->addWidget(proxyInput);

    QDialogButtonBox *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ProxyInputDialog::accept()
{
    std::optional<Core::ProxyConfig> parsed = proxyInput->parseOrWarn();
    if (!parsed)
        return;

    proxy = *parsed;
    QDialog::accept();
}

} // namespace UI
} // namespace Acheron
