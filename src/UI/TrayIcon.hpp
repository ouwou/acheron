#pragma once

#include <QMenu>
#include <QSystemTrayIcon>

namespace Acheron {
namespace UI {

class TrayIcon : public QObject
{
    Q_OBJECT
public:
    explicit TrayIcon(QObject *parent = nullptr);

    void setToolTip(const QString &windowTitle);
    void addMenuAction(QAction *action); // above Quit

signals:
    void showWindowRequested();
    void quitRequested();

private:
    QMenu menu;
    QSystemTrayIcon icon;
};

} // namespace UI
} // namespace Acheron
