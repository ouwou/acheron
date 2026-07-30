#include "SettingsWindow.hpp"

#include "AppearancePage.hpp"
#include "GeneralPage.hpp"

#ifdef ACHERON_HAVE_MINIAUDIO
#include "AudioPage.hpp"
#endif

namespace Acheron {
namespace UI {

SettingsWindow::SettingsWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Settings"));
    resize(550, 400);

    setupUi();
}

void SettingsWindow::setupUi()
{
    auto *mainLayout = new QHBoxLayout(this);

    categoryList = new QListWidget(this);
    categoryList->setFixedWidth(150);

    pages = new QStackedWidget(this);

    auto addPage = [this](const QString &name, QWidget *page) {
        categoryList->addItem(name);
        pages->addWidget(page);
    };

    addPage(tr("General"), new GeneralPage(this));

    auto *appearance = new AppearancePage(this);
    addPage(tr("Appearance"), appearance);
    connect(appearance, &AppearancePage::channelListModeChanged, this, &SettingsWindow::channelListModeChanged);

#ifdef ACHERON_HAVE_MINIAUDIO
    addPage(tr("Audio"), new AudioPage(this));
#endif

    categoryList->setCurrentRow(0);

    connect(categoryList, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    mainLayout->addWidget(categoryList);
    mainLayout->addWidget(pages, 1);
}

} // namespace UI
} // namespace Acheron
