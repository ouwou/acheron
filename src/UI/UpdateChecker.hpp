#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

class QAction;
class QDialog;
class QProcess;
class QWidget;

namespace Acheron {
namespace UI {

// Asks acheron-updater (src/Updater, next to acheron.exe in the Windows CI builds) about new
// nightly builds at launch, every 6 hours and on "Check for Updates", and offers them (#169).
// Does nothing in builds made elsewhere, or when the updater isn't there.
class UpdateChecker : public QObject
{
public:
    // quitApp: how Acheron really quits (not just to the tray) once the updater has been started
    UpdateChecker(QWidget *window, std::function<void()> quitApp);

    [[nodiscard]] bool available() const { return !updaterPath.isEmpty(); }
    // "Check for Updates", for menus; a manual check also says when there's nothing new
    [[nodiscard]] QAction *checkAction() const { return action; }
    void checkNow(bool manual);

private:
    void onCheckFinished(QProcess *process, bool manual);
    void offer(const QJsonObject &build, bool manual);

    QWidget *window;
    std::function<void()> quitApp;
    QString updaterPath;
    QAction *action = nullptr;
    QTimer periodic;
    QPointer<QProcess> running;
    QPointer<QDialog> prompt;
    QString laterRunId; // "Later" this session: automatic checks don't ask about it again
};

} // namespace UI
} // namespace Acheron
