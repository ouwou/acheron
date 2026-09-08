#include "GeneralPage.hpp"

#include <QCheckBox>
#include <QSettings>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

GeneralPage::GeneralPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    inMemoryCacheCheckbox = new QCheckBox(tr("In-memory cache database (requires restart)"), this);
    inMemoryCacheCheckbox->setChecked(QSettings().value("general/in_memory_cache", false).toBool());
    layout->addWidget(inMemoryCacheCheckbox);

    animateEmojiCheckbox = new QCheckBox(tr("Animate emoji"), this);
    animateEmojiCheckbox->setChecked(QSettings().value("chat/animate_emoji", true).toBool());
    layout->addWidget(animateEmojiCheckbox);

    layout->addStretch();

    connect(inMemoryCacheCheckbox, &QCheckBox::toggled, this, [](bool checked) {
        QSettings settings;
        settings.setValue("general/in_memory_cache", checked);
    });

    connect(animateEmojiCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue("chat/animate_emoji", checked);
        emit animateEmojiChanged(checked);
    });
}

} // namespace UI
} // namespace Acheron
