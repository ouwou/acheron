#include "GeneralPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
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

#ifdef Q_OS_WINDOWS
    auto *fontEngineRow = new QHBoxLayout();
    fontEngineRow->addWidget(new QLabel(tr("Font engine (requires restart)"), this));
    fontEngineCombo = new QComboBox(this);
    fontEngineCombo->addItem(tr("DirectWrite (default)"), "directwrite");
    fontEngineCombo->addItem(tr("FreeType"), "freetype");
    fontEngineCombo->setCurrentIndex(qMax(0, fontEngineCombo->findData(QSettings().value("general/font_engine", "directwrite").toString())));
    fontEngineRow->addWidget(fontEngineCombo, 1);
    layout->addLayout(fontEngineRow);

    connect(fontEngineCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        QSettings().setValue("general/font_engine", fontEngineCombo->itemData(index).toString());
    });
#endif

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
