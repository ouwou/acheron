#include "TrayIcon.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QSvgRenderer>

namespace Acheron {
namespace UI {

#ifdef Q_OS_MACOS
static constexpr bool ClickOpensTheMenu = true;
#else
static constexpr bool ClickOpensTheMenu = false;
#endif

static QIcon applicationIcon()
{
    // Fork: prefer the custom tray icon bundled from fork/fork.qrc.
    QIcon custom;
    for (int side : { 16, 24, 32, 48, 64, 128, 256 }) {
        const QString path = QStringLiteral(":/fork/tray-%1.png").arg(side);
        if (QFile::exists(path))
            custom.addFile(path, QSize(side, side));
    }
    if (!custom.isNull())
        return custom;

    QSvgRenderer renderer(QStringLiteral(":/acheron.svg"));
    QIcon rendered;
    for (int side : { 16, 24, 32, 48, 64 }) {
        QPixmap pixmap(side, side);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter);
        painter.end();

        rendered.addPixmap(pixmap);
    }
    return rendered;
}

TrayIcon::TrayIcon(QObject *parent) : QObject(parent), icon(applicationIcon())
{
    menu.addAction(tr("Show Acheron"), this, &TrayIcon::showWindowRequested);
    menu.addSeparator();
    menu.addAction(tr("Quit"), this, &TrayIcon::quitRequested);
    icon.setContextMenu(&menu);

    connect(&icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger && !ClickOpensTheMenu)
            emit showWindowRequested();
    });

    connect(&icon, &QSystemTrayIcon::messageClicked, this, &TrayIcon::messageClicked);

    setToolTip({});
    icon.show();
}

void TrayIcon::showMessage(const QString &title, const QString &body)
{
    static constexpr int SHOWN_MS = 8000;
    icon.showMessage(title, body, icon.icon(), SHOWN_MS);
}

void TrayIcon::setToolTip(const QString &windowTitle)
{
    icon.setToolTip(windowTitle.isEmpty() ? QGuiApplication::applicationDisplayName() : windowTitle);
}

} // namespace UI
} // namespace Acheron
