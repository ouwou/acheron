#include "GeneralPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Core/AnimatedImageCache.hpp"

namespace Acheron {
namespace UI {

static QSpinBox *addMemoryLimit(QVBoxLayout *layout, QWidget *parent, const QString &title, const QString &explanation,
                                const QString &settingsKey, const Core::AnimatedImageCache::MiBRange &range)
{
    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(title, parent));
    auto *spin = new QSpinBox(parent);
    spin->setRange(range.min, range.max);
    spin->setSingleStep(8);
    spin->setSuffix(GeneralPage::tr(" MB"));
    spin->setValue(QSettings().value(settingsKey, range.fallback).toInt());
    row->addWidget(spin);
    row->addStretch();
    layout->addLayout(row);

    auto *help = new QLabel(explanation, parent);
    help->setWordWrap(true);
    help->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(help);

    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), parent, [settingsKey](int megabytes) {
        QSettings().setValue(settingsKey, megabytes);
    });
    return spin;
}

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

    animateStickersCheckbox = new QCheckBox(tr("Animate stickers"), this);
    animateStickersCheckbox->setChecked(QSettings().value("chat/animate_stickers", true).toBool());
    layout->addWidget(animateStickersCheckbox);

    using Core::AnimatedImageCache;
    QSpinBox *cacheLimit = addMemoryLimit(
            layout, this, tr("Memory for animations"),
            tr("Animated emoji and stickers are unpacked into memory so they can play smoothly. This is the most "
               "memory Acheron will use for that. Once it is full, the animations you have not seen for the longest "
               "are dropped, and they take a moment to start moving again the next time they show up. Raise this if "
               "that happens a lot in busy channels, or lower it if Acheron is using more memory than you would like."),
            "chat/animation_cache_mb", AnimatedImageCache::CacheLimit);
    connect(cacheLimit, qOverload<int>(&QSpinBox::valueChanged), this, &GeneralPage::animationCacheLimitChanged);

    QSpinBox *animationLimit = addMemoryLimit(
            layout, this, tr("Largest single animation"),
            tr("A long sticker on a high-resolution screen can need a lot of memory on its own. An animation that "
               "would need more than this is played at a lower resolution instead, and if even that is not enough it "
               "is shown as a still picture."),
            "chat/animation_max_mb", AnimatedImageCache::AnimationLimit);
    connect(animationLimit, qOverload<int>(&QSpinBox::valueChanged), this, &GeneralPage::animationSizeLimitChanged);

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

    connect(animateStickersCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue("chat/animate_stickers", checked);
        emit animateStickersChanged(checked);
    });
}

} // namespace UI
} // namespace Acheron
