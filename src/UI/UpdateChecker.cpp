#include "UpdateChecker.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace Acheron {
namespace UI {

namespace {

constexpr int FirstCheckDelayMs = 15 * 1000;            // not while Acheron is still starting up
constexpr int CheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int CheckTimeoutMs = 2 * 60 * 1000;

// set by CMake in GitHub Actions builds: the run that built this, which is what the updater compares
#ifndef ACHERON_BUILD_RUN_ID
#define ACHERON_BUILD_RUN_ID ""
#endif
#ifndef ACHERON_BUILD_SHA
#define ACHERON_BUILD_SHA ""
#endif

QString updaterBesideApp()
{
    // a local build has nothing to compare against, so it's never offered nightlies
    if (QStringLiteral(ACHERON_BUILD_RUN_ID).isEmpty())
        return {};
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("acheron-updater.exe");
#else
    const QString name = QStringLiteral("acheron-updater");
#endif
    const QString path = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    return QFileInfo::exists(path) ? path : QString();
}

} // namespace

UpdateChecker::UpdateChecker(QWidget *window, std::function<void()> quitApp)
    : QObject(window), window(window), quitApp(std::move(quitApp)), updaterPath(updaterBesideApp())
{
    action = new QAction(tr("Check for &Updates"), this);
    action->setVisible(available());
    connect(action, &QAction::triggered, this, [this] { checkNow(true); });

    if (!available())
        return;
    QTimer::singleShot(FirstCheckDelayMs, this, [this] { checkNow(false); });
    periodic.setInterval(CheckIntervalMs);
    connect(&periodic, &QTimer::timeout, this, [this] { checkNow(false); });
    periodic.start();
}

void UpdateChecker::checkNow(bool manual)
{
    if (!available() || running)
        return;
    if (prompt) {
        prompt->raise();
        prompt->activateWindow();
        return;
    }

    auto *process = new QProcess(this);
    running = process;
    QStringList args { QStringLiteral("--check"), QStringLiteral("--running"), QStringLiteral(ACHERON_BUILD_RUN_ID),
                       QStringLiteral("--running-sha"), QStringLiteral(ACHERON_BUILD_SHA) };
    if (manual)
        args << QStringLiteral("--manual");
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, process, manual] { onCheckFinished(process, manual); });
    connect(process, &QProcess::errorOccurred, this, [this, process, manual](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onCheckFinished(process, manual);
    });
    QTimer::singleShot(CheckTimeoutMs, process, &QProcess::kill);
    process->start(updaterPath, args);
}

void UpdateChecker::onCheckFinished(QProcess *process, bool manual)
{
    const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
    process->deleteLater();

    // an update that failed after Acheron closed for it
    const QString lastError = result.value(QStringLiteral("lastError")).toString();
    if (!lastError.isEmpty())
        QMessageBox::warning(window, tr("Acheron Updater"), tr("The last update didn't install:\n\n%1").arg(lastError));

    const QString status = result.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("update")) {
        if (manual || result.value(QStringLiteral("runId")).toString() != laterRunId)
            offer(result, manual);
    } else if (manual) {
        if (status == QLatin1String("current"))
            QMessageBox::information(window, tr("Acheron Updater"),
                                     tr("Acheron is up to date.\n\n%1").arg(result.value(QStringLiteral("label")).toString()));
        else
            QMessageBox::warning(window, tr("Acheron Updater"),
                                 tr("Couldn't check for updates.\n\n%1")
                                         .arg(result.value(QStringLiteral("error")).toString(process->errorString())));
    }
}

void UpdateChecker::offer(const QJsonObject &build, bool manual)
{
    Q_UNUSED(manual)
    const QString runId = build.value(QStringLiteral("runId")).toString();

    auto *dialog = new QDialog(window);
    prompt = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Acheron update available"));
    dialog->resize(540, 380);

    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(tr("A new Acheron build is available: %1\nInstalled: %2\n\nAcheron will close and reopen to update.")
                                         .arg(build.value(QStringLiteral("label")).toString(),
                                              build.value(QStringLiteral("installed")).toString()),
                                 dialog));
    const QString notWritable = build.value(QStringLiteral("notWritable")).toString();
    if (!notWritable.isEmpty()) {
        auto *warning = new QLabel(notWritable, dialog);
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }
    auto *changes = new QPlainTextEdit(build.value(QStringLiteral("changes")).toString(), dialog);
    changes->setReadOnly(true);
    layout->addWidget(changes, 1);

    auto *buttons = new QDialogButtonBox(dialog);
    QPushButton *skip = buttons->addButton(tr("Skip This Build"), QDialogButtonBox::DestructiveRole);
    QPushButton *later = buttons->addButton(tr("Later"), QDialogButtonBox::RejectRole);
    QPushButton *update = buttons->addButton(tr("Update Now"), QDialogButtonBox::AcceptRole);
    update->setDefault(true);
    update->setEnabled(notWritable.isEmpty());
    layout->addWidget(buttons);

    connect(skip, &QPushButton::clicked, dialog, [this, dialog, runId] {
        QProcess::startDetached(updaterPath, { QStringLiteral("--skip"), runId });
        dialog->close();
    });
    connect(later, &QPushButton::clicked, dialog, [this, dialog, runId] {
        laterRunId = runId;
        dialog->close();
    });
    connect(update, &QPushButton::clicked, dialog, [this, dialog, runId] {
        const QStringList args { QStringLiteral("--install"), runId, QStringLiteral("--wait-pid"),
                                 QString::number(QCoreApplication::applicationPid()), QStringLiteral("--relaunch") };
        if (!QProcess::startDetached(updaterPath, args)) {
            QMessageBox::warning(dialog, tr("Acheron Updater"), tr("Couldn't start the updater."));
            return;
        }
        dialog->close();
        quitApp();
    });

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace UI
} // namespace Acheron
