#include "ConfirmPopup.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QPushButton>

namespace Acheron {
namespace UI {

ConfirmPopup::ConfirmPopup(const QString &title, const QString &text, const QString &yesLabel,
                           QWidget *parent)
    : BasePopup(parent)
{
    auto *layout = new QVBoxLayout(getContainer());
    layout->setSpacing(15);
    layout->setContentsMargins(24, 24, 24, 24);

    if (!title.isEmpty()) {
        auto *titleLabel = new QLabel(title, getContainer());
        QFont titleFont = titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 2);
        titleLabel->setFont(titleFont);

        QPalette p = titleLabel->palette();
        titleLabel->setStyleSheet(QString("color: %1;").arg(p.color(QPalette::WindowText).name()));
        layout->addWidget(titleLabel);

        auto *line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        layout->addWidget(line);
    }

    auto *textLabel = new QLabel(text, getContainer());
    textLabel->setWordWrap(true);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    layout->addWidget(textLabel);

    auto *btnBox = new QDialogButtonBox(getContainer());
    QPushButton *btnYes = btnBox->addButton(yesLabel, QDialogButtonBox::AcceptRole);
    btnBox->addButton(QDialogButtonBox::Cancel);
    btnYes->setDefault(true);

    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    layout->addWidget(btnBox);
}

} // namespace UI
} // namespace Acheron
