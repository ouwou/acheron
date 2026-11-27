// acheron-updater: finds and installs Acheron nightly builds. Fork-only (see fork/README.md).
//
// Lives next to acheron.exe and updates that folder. Windowless: Acheron runs it and shows the prompt.
//
//   --check [--manual]                    print one JSON object about the newest build and exit
//   --skip <runId>                        don't offer that build again (manual checks still do)
//   --install <runId> [--wait-pid <pid>] [--relaunch]
//   --rollback [--wait-pid <pid>] [--relaunch]
//   --source Upstream|Fork  --flavor <name>  --fork-repo <owner/repo>   change where builds come from
//
// State is %LOCALAPPDATA%\AcheronUpdater\state.json, shared with the old PowerShell updater, and the
// log is updater.log next to it. Only Qt Core and Qt Network, which Acheron already ships.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <cstdio>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

const QString UpstreamRepo = QStringLiteral("ouwou/acheron");
const QString DefaultForkRepo = QStringLiteral("RyanBlakeIT/acheron");
const QString Branch = QStringLiteral("master");

// ACHERON_UPDATER_WINDOWS_LAYOUT: only for testing the updater against Windows builds elsewhere
#if defined(Q_OS_WIN) || defined(ACHERON_UPDATER_WINDOWS_LAYOUT)
const QString ExeName = QStringLiteral("acheron.exe");
#else
const QString ExeName = QStringLiteral("acheron");
#endif

// ---------------------------------------------------------------- files, log, state

QString dataDir()
{
    static const QString dir = [] {
        const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/AcheronUpdater");
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

QString installDir()
{
    return QCoreApplication::applicationDirPath();
}

void log(const QString &message)
{
    QFile file(dataDir() + QStringLiteral("/updater.log"));
    if (file.size() > 512 * 1024) {
        QFile::remove(file.fileName() + QStringLiteral(".old"));
        file.rename(file.fileName() + QStringLiteral(".old"));
        file.setFileName(dataDir() + QStringLiteral("/updater.log"));
    }
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss  ")).toUtf8() + message.toUtf8() + '\n');
}

class State
{
public:
    State()
    {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly))
            return;
        QByteArray bytes = file.readAll();
        if (bytes.startsWith("\xEF\xBB\xBF")) // PowerShell 5.1 writes a BOM
            bytes.remove(0, 3);
        obj = QJsonDocument::fromJson(bytes).object();
    }

    [[nodiscard]] QString str(const QString &key) const
    {
        const QJsonValue v = obj.value(key);
        return v.isDouble() ? QString::number(qint64(v.toDouble())) : v.toString();
    }
    [[nodiscard]] QStringList list(const QString &key) const
    {
        QStringList out;
        for (const QJsonValue &v : obj.value(key).toArray())
            out << v.toString();
        return out;
    }
    void set(const QString &key, const QJsonValue &value) { obj.insert(key, value); }
    void set(const QString &key, const QString &value) { obj.insert(key, value.isEmpty() ? QJsonValue() : QJsonValue(value)); }

    bool save() const
    {
        QFile file(path());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        file.write(QJsonDocument(obj).toJson());
        return true;
    }

    [[nodiscard]] bool fork() const { return str(QStringLiteral("Source")) == QLatin1String("Fork"); }
    [[nodiscard]] QString repo() const
    {
        if (!fork())
            return UpstreamRepo;
        const QString r = str(QStringLiteral("ForkRepo"));
        return r.isEmpty() ? DefaultForkRepo : r;
    }
    [[nodiscard]] QString flavor() const;
    [[nodiscard]] QString artifact() const { return QStringLiteral("acheron-windows-") + flavor(); }
    [[nodiscard]] QString workflow() const
    {
        return flavor().startsWith(QLatin1String("arm64-")) ? QStringLiteral("build-windows-arm64.yml") : QStringLiteral("build.yml");
    }

private:
    static QString path() { return dataDir() + QStringLiteral("/state.json"); }
    QJsonObject obj;
};

// PE machine of an .exe: 0x8664 x64, 0xAA64 ARM64
int exeMachine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray head = file.read(4096);
    if (head.size() < 0x40)
        return 0;
    const auto pe = static_cast<quint32>(quint8(head[0x3C]) | quint8(head[0x3D]) << 8 | quint8(head[0x3E]) << 16 | quint8(head[0x3F]) << 24);
    if (pe + 6 > quint32(head.size()))
        return 0;
    return quint8(head[pe + 4]) | quint8(head[pe + 5]) << 8;
}

QString State::flavor() const
{
    const QString saved = str(QStringLiteral("Flavor"));
    if (!saved.isEmpty())
        return saved;
    // what's installed here
    const QDir dir(installDir());
    if (exeMachine(dir.filePath(ExeName)) == 0xAA64)
        return QStringLiteral("arm64-MinSizeRel");
    if (dir.exists(QStringLiteral("libssl-1_1-x64.dll")))
        return QStringLiteral("qt5-MinSizeRel");
    if (dir.entryList({ QStringLiteral("avcodec-*.dll") }, QDir::Files).isEmpty())
        return QStringLiteral("minimal");
    return QStringLiteral("MinSizeRel");
}

// ---------------------------------------------------------------- network

QNetworkAccessManager *network()
{
    static QNetworkAccessManager *nam = [] {
        // the system's proxy settings, as on managed networks
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        auto *n = new QNetworkAccessManager(QCoreApplication::instance());
        n->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        return n;
    }();
    return nam;
}

// blocking GET; with sink, the body is streamed into it
std::optional<QByteArray> get(const QString &url, QString *error, int timeoutMs = 30000, QIODevice *sink = nullptr)
{
    QNetworkRequest request{ QUrl(url) };
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("acheron-updater"));
    if (url.startsWith(QLatin1String("https://api.github.com/"))) {
        request.setRawHeader("Accept", "application/vnd.github+json");
        const QByteArray token = qgetenv("GITHUB_TOKEN");
        if (!token.isEmpty())
            request.setRawHeader("Authorization", "Bearer " + token);
    }

    QNetworkReply *reply = network()->get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    // a stall, not the whole transfer: restarted whenever data arrives
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        timeout.start(timeoutMs);
        if (sink)
            sink->write(reply->readAll());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    loop.exec();

    std::optional<QByteArray> result;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
        const QByteArray rest = reply->readAll();
        if (sink)
            sink->write(rest);
        result = sink ? QByteArray() : rest;
    } else if (error) {
        *error = status ? QStringLiteral("HTTP %1 from %2").arg(status).arg(QUrl(url).host())
                        : reply->errorString();
    }
    reply->deleteLater();
    return result;
}

std::optional<QJsonObject> getJson(const QString &url, QString *error)
{
    const auto body = get(url, error);
    if (!body)
        return std::nullopt;
    return QJsonDocument::fromJson(*body).object();
}

// ---------------------------------------------------------------- builds

struct Build
{
    QString runId;
    int runNumber = 0;
    QString sha;
    QString message;
    QString url;
    QDateTime updated;
    QString digest; // sha256 hex of the artifact zip, when GitHub has it

    [[nodiscard]] QString label() const
    {
        return QStringLiteral("build #%1, %2 (commit %3)")
                .arg(runNumber)
                .arg(updated.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")), sha.left(7));
    }
};

// the run's artifact for this flavor, if it uploaded one that hasn't expired
std::optional<QString> artifactDigest(const State &state, const QString &runId, QString *error)
{
    const auto arts = getJson(QStringLiteral("https://api.github.com/repos/%1/actions/runs/%2/artifacts?name=%3")
                                      .arg(state.repo(), runId, state.artifact()),
                              error);
    if (!arts)
        return std::nullopt;
    for (const QJsonValue &a : arts->value(QStringLiteral("artifacts")).toArray()) {
        const QJsonObject artifact = a.toObject();
        if (!artifact.value(QStringLiteral("expired")).toBool())
            return artifact.value(QStringLiteral("digest")).toString().remove(QStringLiteral("sha256:")).toLower();
    }
    return std::nullopt;
}

Build buildFromRun(const QJsonObject &run)
{
    Build b;
    b.runId = QString::number(qint64(run.value(QStringLiteral("id")).toDouble()));
    b.runNumber = run.value(QStringLiteral("run_number")).toInt();
    b.sha = run.value(QStringLiteral("head_sha")).toString();
    b.message = run.value(QStringLiteral("head_commit")).toObject().value(QStringLiteral("message")).toString();
    b.url = run.value(QStringLiteral("html_url")).toString();
    b.updated = QDateTime::fromString(run.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);
    return b;
}

// newest finished push build on master that produced this flavor; a run counts even if an unrelated job failed
std::optional<Build> latestBuild(const State &state, QString *error)
{
    const auto runs = getJson(QStringLiteral("https://api.github.com/repos/%1/actions/workflows/%2/runs?branch=%3&event=push&status=completed&per_page=10")
                                      .arg(state.repo(), state.workflow(), Branch),
                              error);
    if (!runs)
        return std::nullopt;
    for (const QJsonValue &r : runs->value(QStringLiteral("workflow_runs")).toArray()) {
        const QJsonObject run = r.toObject();
        Build b = buildFromRun(run);
        const auto digest = artifactDigest(state, b.runId, error);
        if (!digest)
            continue;
        b.digest = *digest;
        return b;
    }
    if (error && error->isEmpty())
        *error = QStringLiteral("No finished build of %1 found in %2").arg(state.artifact(), state.repo());
    return std::nullopt;
}

std::optional<Build> buildByRunId(const State &state, const QString &runId, QString *error)
{
    const auto run = getJson(QStringLiteral("https://api.github.com/repos/%1/actions/runs/%2").arg(state.repo(), runId), error);
    if (!run)
        return std::nullopt;
    Build b = buildFromRun(*run);
    const auto digest = artifactDigest(state, runId, error);
    if (!digest) {
        if (error && error->isEmpty())
            *error = QStringLiteral("Build %1 has no %2 download").arg(runId, state.artifact());
        return std::nullopt;
    }
    b.digest = *digest;
    return b;
}

QString changeSummary(const State &state, const Build &latest)
{
    QStringList lines;
    const QString installedSha = state.str(QStringLiteral("Sha"));
    if (!installedSha.isEmpty() && installedSha != latest.sha && state.str(QStringLiteral("RepoOfRunId")) == state.repo()) {
        QString error;
        if (const auto cmp = getJson(QStringLiteral("https://api.github.com/repos/%1/compare/%2...%3").arg(state.repo(), installedSha, latest.sha), &error)) {
            const QJsonArray commits = cmp->value(QStringLiteral("commits")).toArray();
            const int total = cmp->value(QStringLiteral("total_commits")).toInt();
            lines << QStringLiteral("%1 new commit(s) since your build:").arg(total) << QString();
            for (int i = commits.size() - 1, shown = 0; i >= 0 && shown < 30; i--, shown++) {
                const QJsonObject c = commits[i].toObject();
                const QString subject = c.value(QStringLiteral("commit")).toObject().value(QStringLiteral("message")).toString().section(QLatin1Char('\n'), 0, 0).trimmed();
                lines << QStringLiteral("- %1  [%2]").arg(subject, c.value(QStringLiteral("sha")).toString().left(7));
            }
            if (total > 30)
                lines << QStringLiteral("- ...and %1 more").arg(total - 30);
        }
    }
    if (lines.isEmpty() && !latest.message.isEmpty())
        lines << QStringLiteral("Latest commit:") << QString() << latest.message.trimmed();
    lines << QString() << QStringLiteral("Build details: ") + latest.url;
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------- file swapping

// in-use files (this updater, Qt DLLs it loaded) can't be overwritten on Windows, but can be renamed aside
bool replaceFile(const QString &source, const QString &target)
{
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (QFile::exists(target) && !QFile::remove(target)) {
        QString aside = target + QStringLiteral(".old");
        for (int n = 1; QFile::exists(aside) && !QFile::remove(aside); n++)
            aside = target + QStringLiteral(".old%1").arg(n);
        if (!QFile::rename(target, aside))
            return false;
    }
    return QFile::copy(source, target);
}

QStringList filesUnder(const QString &root, bool skipAside = true)
{
    QStringList out;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        const QString path = it.next();
        if (skipAside && QFileInfo(path).suffix().startsWith(QLatin1String("old")))
            continue;
        out << QDir::toNativeSeparators(base.relativeFilePath(path));
    }
    return out;
}

void removeAsideFiles()
{
    QDirIterator it(installDir(), { QStringLiteral("*.old"), QStringLiteral("*.old?"), QStringLiteral("*.old??") }, QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        QFile::remove(it.next()); // still in use: next time
}

// copies everything from source over target; files listed in staleCandidates but not shipped any more are removed
bool overlay(const QString &source, const QString &target, const QStringList &shipped, const QStringList &staleCandidates, QString *error)
{
    for (const QString &rel : shipped) {
        if (!replaceFile(QDir(source).filePath(rel), QDir(target).filePath(rel))) {
            *error = QStringLiteral("Couldn't replace %1 (in use?)").arg(rel);
            return false;
        }
    }
    QSet<QString> keep;
    for (const QString &rel : shipped)
        keep.insert(QString(rel).replace(QLatin1Char('\\'), QLatin1Char('/')).toLower());
    for (const QString &rel : staleCandidates) {
        if (keep.contains(QString(rel).replace(QLatin1Char('\\'), QLatin1Char('/')).toLower()))
            continue;
        // the PowerShell updater wrote these with backslashes
        const QString relative = QString(rel).replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString path = QDir(target).filePath(relative);
        if (QFile::exists(path) && !QFile::remove(path))
            QFile::rename(path, path + QStringLiteral(".old"));
        // and folders left empty
        QDir parent = QFileInfo(path).dir();
        while (QDir::cleanPath(parent.absolutePath()) != QDir::cleanPath(target) && parent.isEmpty() && parent.cdUp())
            parent.rmdir(QFileInfo(path).dir().dirName()), path.isEmpty();
    }
    return true;
}

bool copyTree(const QString &source, const QString &target)
{
    QDir(target).removeRecursively();
    for (const QString &rel : filesUnder(source)) {
        const QString to = QDir(target).filePath(QDir::fromNativeSeparators(rel));
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QFile::copy(QDir(source).filePath(QDir::fromNativeSeparators(rel)), to))
            return false;
    }
    return true;
}

bool extractZip(const QString &zip, const QString &dir, QString *error)
{
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    QProcess p;
#ifdef Q_OS_WIN
    // Windows 10+ ships bsdtar, which reads zips
    p.start(QStringLiteral("tar"), { QStringLiteral("-xf"), zip, QStringLiteral("-C"), dir });
#else
    p.start(QStringLiteral("unzip"), { QStringLiteral("-q"), QStringLiteral("-o"), zip, QStringLiteral("-d"), dir });
#endif
    if (!p.waitForFinished(300000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        *error = QStringLiteral("Couldn't unpack the download: ") + QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

// the folder in the unpacked download that holds the program
QString programRoot(const QString &dir)
{
    if (QFile::exists(QDir(dir).filePath(ExeName)))
        return dir;
    for (const QString &sub : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFile::exists(QDir(dir).filePath(sub + QLatin1Char('/') + ExeName)))
            return QDir(dir).filePath(sub);
    }
    return {};
}

bool waitForExit(qint64 pid, int timeoutMs)
{
    if (pid <= 0)
        return true;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return true; // already gone
    const bool exited = WaitForSingleObject(process, DWORD(timeoutMs)) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
#else
    for (int waited = 0; waited < timeoutMs; waited += 200) {
        if (kill(pid_t(pid), 0) != 0)
            return true;
        QThread::msleep(200);
    }
    return false;
#endif
}

void relaunch()
{
    const QString exe = QDir(installDir()).filePath(ExeName);
    if (!QProcess::startDetached(exe, {}, installDir()))
        log(QStringLiteral("Couldn't start ") + exe);
}

// ---------------------------------------------------------------- commands

void print(const QJsonObject &obj)
{
    const QByteArray out = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    fwrite(out.constData(), 1, size_t(out.size()), stdout);
    fflush(stdout);
}

int check(State &state, bool manual)
{
    removeAsideFiles();

    QJsonObject out;
    const QString lastError = state.str(QStringLiteral("LastError"));
    if (!lastError.isEmpty()) {
        out[QStringLiteral("lastError")] = lastError;
        state.set(QStringLiteral("LastError"), QString());
        state.save();
    }

    QString error;
    const auto latest = latestBuild(state, &error);
    if (!latest) {
        log(QStringLiteral("Check failed: ") + error);
        out[QStringLiteral("status")] = QStringLiteral("error");
        out[QStringLiteral("error")] = error;
        print(out);
        return 1;
    }

    const QString here = QDir::toNativeSeparators(installDir());
    const bool sameFolder = state.str(QStringLiteral("InstalledTo")).isEmpty() ||
                            QDir::toNativeSeparators(state.str(QStringLiteral("InstalledTo"))).compare(here, Qt::CaseInsensitive) == 0;
    const bool sameFlavor = state.str(QStringLiteral("InstalledFlavor")).isEmpty() || state.str(QStringLiteral("InstalledFlavor")) == state.flavor();
    const bool current = sameFolder && sameFlavor && state.str(QStringLiteral("RunId")) == latest->runId;

    out[QStringLiteral("runId")] = latest->runId;
    out[QStringLiteral("label")] = latest->label();
    out[QStringLiteral("url")] = latest->url;
    out[QStringLiteral("source")] = state.repo();
    out[QStringLiteral("flavor")] = state.flavor();
    if (current) {
        out[QStringLiteral("status")] = QStringLiteral("current");
    } else if (!manual && state.str(QStringLiteral("SkippedRunId")) == latest->runId) {
        out[QStringLiteral("status")] = QStringLiteral("skipped");
    } else {
        out[QStringLiteral("status")] = QStringLiteral("update");
        const QString installedRun = state.str(QStringLiteral("RunId"));
        out[QStringLiteral("installed")] = installedRun.isEmpty() ? QStringLiteral("unknown")
                                                                  : QStringLiteral("run %1 (commit %2)").arg(installedRun, state.str(QStringLiteral("Sha")).left(7));
        out[QStringLiteral("changes")] = changeSummary(state, *latest);
    }
    print(out);
    return 0;
}

int fail(State &state, const QString &error, bool relaunchAfter)
{
    log(QStringLiteral("ERROR: ") + error);
    state.set(QStringLiteral("LastError"), error);
    state.save();
    if (relaunchAfter)
        relaunch();
    return 1;
}

int install(State &state, const QString &runId, qint64 waitPid, bool relaunchAfter)
{
    log(QStringLiteral("Installing run %1 of %2 (%3) into %4").arg(runId, state.repo(), state.artifact(), installDir()));
    QString error;
    const auto build = buildByRunId(state, runId, &error);
    if (!build)
        return fail(state, error, relaunchAfter && waitForExit(waitPid, 30000));

    // download while Acheron closes
    const QString downloads = dataDir() + QStringLiteral("/download");
    QDir(downloads).removeRecursively();
    QDir().mkpath(downloads);
    const QString zipPath = downloads + QStringLiteral("/%1-%2.zip").arg(state.artifact(), runId);
    const QString url = QStringLiteral("https://nightly.link/%1/actions/runs/%2/%3.zip").arg(state.repo(), runId, state.artifact());
    log(QStringLiteral("Downloading ") + url);
    bool downloaded = false;
    for (int attempt = 1; attempt <= 3 && !downloaded; attempt++) {
        QFile zip(zipPath);
        if (!zip.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(state, QStringLiteral("Couldn't write ") + zipPath, relaunchAfter && waitForExit(waitPid, 30000));
        downloaded = get(url, &error, 60000, &zip).has_value();
        if (!downloaded)
            QThread::sleep(5UL * attempt);
    }
    if (!downloaded)
        return fail(state, QStringLiteral("Download failed: ") + error, relaunchAfter && waitForExit(waitPid, 30000));

    {
        QFile zip(zipPath);
        zip.open(QIODevice::ReadOnly);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&zip);
        const QString actual = QString::fromLatin1(hash.result().toHex());
        if (!build->digest.isEmpty() && actual != build->digest)
            return fail(state, QStringLiteral("Download failed verification (SHA-256 %1, GitHub says %2)").arg(actual, build->digest),
                        relaunchAfter && waitForExit(waitPid, 30000));
        log(build->digest.isEmpty() ? QStringLiteral("SHA-256 %1 (GitHub has no digest to compare)").arg(actual)
                                    : QStringLiteral("SHA-256 verified: ") + actual);
    }

    const QString staging = dataDir() + QStringLiteral("/staging");
    if (!extractZip(zipPath, staging, &error))
        return fail(state, error, relaunchAfter && waitForExit(waitPid, 30000));
    QFile::remove(zipPath);
    const QString root = programRoot(staging);
    if (root.isEmpty())
        return fail(state, QStringLiteral("The download doesn't contain ") + ExeName, relaunchAfter && waitForExit(waitPid, 30000));

    if (!waitForExit(waitPid, 30000))
        return fail(state, QStringLiteral("Acheron didn't close, so nothing was changed"), false);

    // keep the build being replaced, for --rollback
    const QString backup = dataDir() + QStringLiteral("/previous");
    if (QFile::exists(QDir(installDir()).filePath(ExeName)) && !copyTree(installDir(), backup))
        return fail(state, QStringLiteral("Couldn't back up the current build to ") + backup, relaunchAfter);

    const QStringList shipped = filesUnder(root, false);
    if (!overlay(root, installDir(), shipped, state.list(QStringLiteral("Manifest")), &error)) {
        QString restoreError;
        overlay(backup, installDir(), filesUnder(backup), {}, &restoreError);
        return fail(state, error + QStringLiteral(" (the previous build was put back)"), relaunchAfter);
    }
    QDir(staging).removeRecursively();

    state.set(QStringLiteral("PreviousRunId"), state.str(QStringLiteral("RunId")));
    state.set(QStringLiteral("PreviousSha"), state.str(QStringLiteral("Sha")));
    state.set(QStringLiteral("PreviousRepo"), state.str(QStringLiteral("RepoOfRunId")));
    state.set(QStringLiteral("RunId"), build->runId);
    state.set(QStringLiteral("Sha"), build->sha);
    state.set(QStringLiteral("RepoOfRunId"), state.repo());
    state.set(QStringLiteral("InstallDir"), QDir::toNativeSeparators(installDir()));
    state.set(QStringLiteral("InstalledTo"), QDir::toNativeSeparators(installDir()));
    state.set(QStringLiteral("InstalledFlavor"), state.flavor());
    state.set(QStringLiteral("InstalledAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    state.set(QStringLiteral("Manifest"), QJsonArray::fromStringList(shipped));
    state.set(QStringLiteral("SkippedRunId"), QString());
    state.save();
    log(QStringLiteral("Installed ") + build->label());

    if (relaunchAfter)
        relaunch();
    return 0;
}

int rollback(State &state, qint64 waitPid, bool relaunchAfter)
{
    const QString backup = dataDir() + QStringLiteral("/previous");
    if (!QFile::exists(QDir(backup).filePath(ExeName)))
        return fail(state, QStringLiteral("There's no previous build to go back to"), relaunchAfter && waitForExit(waitPid, 30000));
    if (!waitForExit(waitPid, 30000))
        return fail(state, QStringLiteral("Acheron didn't close, so nothing was changed"), false);

    QString error;
    const QStringList files = filesUnder(backup);
    if (!overlay(backup, installDir(), files, state.list(QStringLiteral("Manifest")), &error))
        return fail(state, error, relaunchAfter);

    const QString bad = state.str(QStringLiteral("RunId"));
    state.set(QStringLiteral("RunId"), state.str(QStringLiteral("PreviousRunId")));
    state.set(QStringLiteral("Sha"), state.str(QStringLiteral("PreviousSha")));
    state.set(QStringLiteral("RepoOfRunId"), state.str(QStringLiteral("PreviousRepo")));
    state.set(QStringLiteral("PreviousRunId"), QString());
    state.set(QStringLiteral("PreviousSha"), QString());
    state.set(QStringLiteral("Manifest"), QJsonArray::fromStringList(files));
    state.set(QStringLiteral("SkippedRunId"), bad); // don't offer the build just rolled back from
    state.save();
    log(QStringLiteral("Rolled back to the previous build"));
    if (relaunchAfter)
        relaunch();
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    args.removeFirst();
    auto value = [&](const QString &flag) {
        const int i = args.indexOf(flag);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
    };

    State state;
    bool changed = false;
    if (const QString source = value(QStringLiteral("--source")); source == QLatin1String("Fork") || source == QLatin1String("Upstream")) {
        if (state.str(QStringLiteral("Source")) != source)
            state.set(QStringLiteral("SkippedRunId"), QString()); // a different repo's run IDs
        state.set(QStringLiteral("Source"), source);
        changed = true;
    }
    if (const QString repo = value(QStringLiteral("--fork-repo")); !repo.isEmpty()) {
        state.set(QStringLiteral("ForkRepo"), repo);
        changed = true;
    }
    if (const QString flavor = value(QStringLiteral("--flavor")); !flavor.isEmpty()) {
        state.set(QStringLiteral("Flavor"), flavor);
        changed = true;
    }
    if (changed)
        state.save();

    const qint64 waitPid = value(QStringLiteral("--wait-pid")).toLongLong();
    const bool relaunchAfter = args.contains(QStringLiteral("--relaunch"));

    if (args.contains(QStringLiteral("--check")))
        return check(state, args.contains(QStringLiteral("--manual")));
    if (const QString runId = value(QStringLiteral("--skip")); !runId.isEmpty()) {
        state.set(QStringLiteral("SkippedRunId"), runId);
        return state.save() ? 0 : 1;
    }
    if (const QString runId = value(QStringLiteral("--install")); !runId.isEmpty())
        return install(state, runId, waitPid, relaunchAfter);
    if (args.contains(QStringLiteral("--rollback")))
        return rollback(state, waitPid, relaunchAfter);
    return changed ? 0 : 2;
}
