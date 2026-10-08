#include "TrayIcon.hpp"

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

    setToolTip({});
    icon.show();
}

void TrayIcon::setToolTip(const QString &windowTitle)
{
    icon.setToolTip(windowTitle.isEmpty() ? QGuiApplication::applicationDisplayName() : windowTitle);
}

} // namespace UI
} // namespace Acheron
