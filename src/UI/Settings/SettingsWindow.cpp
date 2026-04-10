#include "SettingsWindow.hpp"

#include <QSettings>

namespace Acheron {
namespace UI {

SettingsWindow::SettingsWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(tr("Settings"));
    resize(550, 400);

    setupUi();
    loadSettings();
}

void SettingsWindow::setupUi()
{
    auto *mainLayout = new QHBoxLayout(this);

    categoryList = new QListWidget(this);
    categoryList->setFixedWidth(150);
    categoryList->addItem(tr("General"));
    categoryList->setCurrentRow(0);

    pages = new QStackedWidget(this);

    auto *generalPage = new QWidget(this);
    auto *generalLayout = new QVBoxLayout(generalPage);

    inMemoryCacheCheckbox = new QCheckBox(tr("In-memory cache database (requires restart)"), generalPage);
    generalLayout->addWidget(inMemoryCacheCheckbox);
    generalLayout->addStretch();

    pages->addWidget(generalPage);

    connect(categoryList, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    connect(inMemoryCacheCheckbox, &QCheckBox::toggled, this, [](bool checked) {
        QSettings settings;
        settings.setValue("general/in_memory_cache", checked);
    });

    mainLayout->addWidget(categoryList);
    mainLayout->addWidget(pages, 1);
}

void SettingsWindow::loadSettings()
{
    QSettings settings;
    inMemoryCacheCheckbox->setChecked(settings.value("general/in_memory_cache", false).toBool());
}

} // namespace UI
} // namespace Acheron
