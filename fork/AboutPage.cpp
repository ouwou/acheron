#include "AboutPage.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <curl/curl.h>

// Filled in by fork/fork.cmake at configure time (from GitHub Actions or the local git checkout).
#ifndef ACHERON_FORK_COMMIT
#define ACHERON_FORK_COMMIT ""
#endif
#ifndef ACHERON_FORK_REPO
#define ACHERON_FORK_REPO ""
#endif
#ifndef ACHERON_FORK_RUN_NUMBER
#define ACHERON_FORK_RUN_NUMBER ""
#endif
#ifndef ACHERON_FORK_RUN_ID
#define ACHERON_FORK_RUN_ID ""
#endif
#ifndef ACHERON_FORK_WORKFLOW
#define ACHERON_FORK_WORKFLOW ""
#endif
#ifndef ACHERON_FORK_BUILD_TIME
#define ACHERON_FORK_BUILD_TIME ""
#endif
#ifndef ACHERON_FORK_BUILD_CONFIG
#define ACHERON_FORK_BUILD_CONFIG ""
#endif

namespace Acheron {
namespace UI {

namespace {

QString fromBuild(const char *value)
{
    return QString::fromUtf8(value).trimmed();
}

QString link(const QString &url, const QString &text)
{
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), text.toHtmlEscaped());
}

} // namespace

AboutPage::AboutPage(QWidget *parent) : QWidget(parent)
{
    const QString repo = fromBuild(ACHERON_FORK_REPO);
    const QString commit = fromBuild(ACHERON_FORK_COMMIT);
    const QString runNumber = fromBuild(ACHERON_FORK_RUN_NUMBER);
    const QString runId = fromBuild(ACHERON_FORK_RUN_ID);
    const QString workflow = fromBuild(ACHERON_FORK_WORKFLOW);
    const QString config = fromBuild(ACHERON_FORK_BUILD_CONFIG);
    const QString github = repo.isEmpty() ? QString() : QStringLiteral("https://github.com/") + repo;

    auto *layout = new QVBoxLayout(this);

    // Header: icon, name, where it came from
    auto *header = new QHBoxLayout;
    auto *logo = new QLabel(this);
    logo->setPixmap(QIcon(QStringLiteral(":/fork/tray-64.png")).pixmap(QSize(64, 64)));
    header->addWidget(logo, 0, Qt::AlignTop);
    auto *title = new QLabel(this);
    title->setTextFormat(Qt::RichText);
    title->setOpenExternalLinks(true);
    title->setText(QStringLiteral("<span style=\"font-size:16pt; font-weight:600\">Acheron</span><br>%1<br>%2")
                           .arg(repo.isEmpty() ? tr("Local build") : link(github, repo),
                                tr("Based on %1").arg(link(QStringLiteral("https://github.com/ouwou/acheron"),
                                                           QStringLiteral("ouwou/acheron")))));
    header->addWidget(title, 1);
    layout->addLayout(header);
    layout->addSpacing(8);

    // Details. Each row is kept as plain text too, for "Copy build info".
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    QStringList plain;
    auto addRow = [&](const QString &name, const QString &text, const QString &rich = QString()) {
        auto *value = new QLabel(rich.isEmpty() ? text.toHtmlEscaped() : rich, this);
        value->setTextFormat(Qt::RichText);
        value->setTextInteractionFlags(Qt::TextBrowserInteraction);
        value->setOpenExternalLinks(true);
        value->setWordWrap(true);
        form->addRow(name + QLatin1Char(':'), value);
        plain << name + QStringLiteral(": ") + text;
    };

    if (!runNumber.isEmpty()) {
        const QString text = workflow.isEmpty() ? tr("#%1").arg(runNumber) : tr("%1 #%2").arg(workflow, runNumber);
        const QString runUrl = (!github.isEmpty() && !runId.isEmpty()) ? github + QStringLiteral("/actions/runs/") + runId : QString();
        addRow(tr("Build"), text, runUrl.isEmpty() ? QString() : link(runUrl, text));
    } else {
        addRow(tr("Build"), tr("Local build"));
    }

    const QDateTime built = QDateTime::fromString(fromBuild(ACHERON_FORK_BUILD_TIME), Qt::ISODate);
    if (built.isValid())
        addRow(tr("Built"), QLocale().toString(built.toLocalTime(), QLocale::LongFormat));

    if (!commit.isEmpty()) {
        const QString shortCommit = commit.left(7);
        addRow(tr("Commit"), commit,
               github.isEmpty() ? QString() : link(github + QStringLiteral("/commit/") + commit, shortCommit));
    }

    addRow(tr("Configuration"),
           QStringLiteral("%1, %2").arg(config.isEmpty() ? tr("unknown") : config, QSysInfo::buildCpuArchitecture()));

    QStringList features;
#ifndef ACHERON_NO_VOICE
    features << tr("voice");
#endif
#ifdef ACHERON_HAVE_VIDEO
    features << tr("video");
#endif
    addRow(tr("Features"), features.isEmpty() ? tr("none (minimal build)") : features.join(QStringLiteral(", ")));

    addRow(tr("Qt"), QStringLiteral("%1 (built with %2)").arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR)));

    if (const curl_version_info_data *curl = curl_version_info(CURLVERSION_NOW))
        addRow(tr("libcurl"), QString::fromLatin1(curl->version));

    QString system = QSysInfo::prettyProductName();
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu != QSysInfo::buildCpuArchitecture())
        system += tr(" (%1 CPU, running under emulation)").arg(cpu);
    addRow(tr("System"), system);

    layout->addLayout(form);
    layout->addStretch(1);

    // Copy everything above, for bug reports
    auto *copy = new QPushButton(tr("Copy build info"), this);
    const QString summary = QStringLiteral("Acheron %1\n").arg(repo.isEmpty() ? tr("(local build)") : repo)
                            + plain.join(QLatin1Char('\n'));
    connect(copy, &QPushButton::clicked, this, [copy, summary] {
        QGuiApplication::clipboard()->setText(summary);
        copy->setText(QObject::tr("Copied"));
        QTimer::singleShot(1500, copy, [copy] { copy->setText(QObject::tr("Copy build info")); });
    });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(copy);
    layout->addLayout(buttons);
}

} // namespace UI
} // namespace Acheron
