#include "CurlUtils.hpp"

#include "ClientIdentity.hpp"
#include "Core/Logging.hpp"
#include "Core/NetworkRequest.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrl>
#include <QRandomGenerator>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

#include <algorithm>
#include <utility>
#include <vector>

namespace Acheron {
namespace Discord {
namespace CurlUtils {

static int cachedBuildNumber = 0;
static bool buildNumberFetchInFlight = false;
static std::vector<std::function<void()>> buildNumberWaiters;
static constexpr int fallbackBuildNumber = 600539;
static constexpr int BUILD_NUMBER_TIMEOUT_MS = 10000;

static void resolveBuildNumberWaiters()
{
    buildNumberFetchInFlight = false;

    std::vector<std::function<void()>> waiters;
    waiters.swap(buildNumberWaiters);
    for (auto &waiter : waiters)
        waiter();
}

void ensureBuildNumber(QNetworkAccessManager *nam, std::function<void()> done)
{
    if (cachedBuildNumber > 0) {
        done();
        return;
    }

    buildNumberWaiters.push_back(std::move(done));
    if (buildNumberFetchInFlight)
        return;
    buildNumberFetchInFlight = true;

    qCInfo(LogNetwork) << "Fetching Discord build number...";

    QNetworkRequest request = Core::networkRequest(QUrl("https://discord.com/app"));
    request.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());
    request.setTransferTimeout(BUILD_NUMBER_TIMEOUT_MS);

    QNetworkReply *appReply = nam->get(request);
    QObject::connect(appReply, &QNetworkReply::finished, [nam, appReply]() {
        appReply->deleteLater();

        if (appReply->error() != QNetworkReply::NoError) {
            qCWarning(LogNetwork) << "Failed to fetch Discord app page:" << appReply->errorString();
            resolveBuildNumberWaiters();
            return;
        }

        QByteArray appPage = appReply->readAll();
        QRegularExpression sentryRegex("/assets/sentry.*?\\.js");
        QRegularExpressionMatch sentryMatch = sentryRegex.match(appPage);

        if (!sentryMatch.hasMatch()) {
            qCWarning(LogNetwork) << "Failed to find sentry JS path";
            resolveBuildNumberWaiters();
            return;
        }

        QString sentryPath = sentryMatch.captured(0);
        QString sentryUrl = "https://discord.com" + sentryPath;

        QNetworkRequest sentryRequest = Core::networkRequest(QUrl(sentryUrl));
        sentryRequest.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());
        sentryRequest.setTransferTimeout(BUILD_NUMBER_TIMEOUT_MS);

        QNetworkReply *sentryReply = nam->get(sentryRequest);
        QObject::connect(sentryReply, &QNetworkReply::finished, [sentryReply]() {
            sentryReply->deleteLater();

            QByteArray sentryJs = sentryReply->readAll();
            QRegularExpression buildRegex("buildNumber\",\"(\\d+)");
            QRegularExpressionMatch buildMatch = buildRegex.match(sentryJs);

            if (sentryReply->error() != QNetworkReply::NoError)
                qCWarning(LogNetwork) << "Failed to fetch sentry JS:" << sentryReply->errorString();
            else if (!buildMatch.hasMatch())
                qCWarning(LogNetwork) << "Failed to extract build number";
            else {
                cachedBuildNumber = buildMatch.captured(1).toInt();
                qCInfo(LogNetwork) << "Discord build number:" << cachedBuildNumber;
            }

            resolveBuildNumberWaiters();
        });
    });
}

int getBuildNumber()
{
    if (cachedBuildNumber > 0)
        return cachedBuildNumber;
    return fallbackBuildNumber;
}

QString getCertificatePath()
{
    QString execDir = QCoreApplication::applicationDirPath();
    QString certPath = QDir(execDir).filePath("certs/cacert.pem");
    if (QFile::exists(certPath))
        return certPath;

    qCWarning(LogNetwork) << "Certificate file not found at" << certPath;
    return QString();
}

static QByteArray readCertificateBundle()
{
    const QString certPath = getCertificatePath();
    if (certPath.isEmpty())
        return {};

    QFile file(certPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(LogNetwork) << "Cannot read certificate file" << certPath << ":" << file.errorString();
        return {};
    }
    return file.readAll();
}

// curl-impersonate's chrome150 tls measured the same as chrome 158 on windows, so the strings are 158's.
// v2.2.0+ reorder custom headers, so newer profiles need a fix there first
QString getImpersonateTarget()
{
    return "chrome150";
}

QString getUserAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/158.0.0.0 Safari/537.36";
}

static QByteArray getSecChUa()
{
    return R"("Chromium";v="158", "Not.A/Brand";v="24", "Google Chrome";v="158")";
}

UserAgentProps getUserAgentProps()
{
    return { "Windows", "Chrome", "158.0.0.0", "10" };
}

// chrome's default: its ui language (the windows one), that language's base, then en-US, en
static QStringList browserLanguages()
{
    // name() drops the script qt 6 puts in ui languages (en-Latn-US)
    const QString language = QLocale(QLocale::system().uiLanguages().value(0, "en-US")).name().replace('_', '-');
    QStringList languages;
    if (!language.isEmpty() && language != "C")
        languages.append(language);
    const QString base = language.section('-', 0, 0);
    for (const QString &fallback : { base, QString("en-US"), QString("en") }) {
        if (!fallback.isEmpty() && fallback != "C" && !languages.contains(fallback))
            languages.append(fallback);
    }
    return languages;
}

QString getSystemLocale()
{
    static const QString locale = browserLanguages().first();
    return locale;
}

QByteArray getAcceptLanguage()
{
    static const QByteArray header = [] {
        QByteArray value;
        int q = 10;
        for (const QString &language : browserLanguages()) {
            if (!value.isEmpty())
                value += ',';
            value += language.toUtf8();
            if (q < 10)
                value += ";q=0." + QByteArray::number(q);
            q = std::max(q - 1, 1);
        }
        return value;
    }();
    return header;
}

void applyCommonOptions(CURL *curl)
{
    static const QByteArray caBundle = readCertificateBundle();
    if (!caBundle.isEmpty()) {
        curl_blob blob;
        blob.data = const_cast<char *>(caBundle.constData());
        blob.len = static_cast<size_t>(caBundle.size());
        blob.flags = CURL_BLOB_NOCOPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
#ifdef IS_CURL_IMPERSONATE
    // its default headers are a mac chrome page load, every request builds its own
    const CURLcode impersonated = curl_easy_impersonate(curl, getImpersonateTarget().toUtf8().constData(), 0);
    static const bool logged = [impersonated] {
        if (impersonated == CURLE_OK)
            qCInfo(LogNetwork) << "Impersonating" << getImpersonateTarget();
        else
            qCWarning(LogNetwork) << "curl_easy_impersonate failed:" << curl_easy_strerror(impersonated);
        return true;
    }();
#else
    static const bool logged = [] {
        qCWarning(LogNetwork) << "Built without curl-impersonate, TLS does not look like Chrome";
        return true;
    }();
#endif
    Q_UNUSED(logged);
}

void applyProxy(CURL *curl, const Core::ProxyConfig &proxy)
{
    if (!proxy.enabled())
        return;

    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.toCurlUrl().toUtf8().constData());
    // ignore $no_proxy
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "");

    if (proxy.type == Core::ProxyConfig::Type::Https) {
        CURLcode rc = curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
        if (rc != CURLE_OK)
            qCWarning(LogNetwork) << "libcurl lacks HTTPS-proxy support:" << curl_easy_strerror(rc);
    }

    if (!proxy.username.isEmpty()) {
        curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy.username.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy.password.toUtf8().constData());
    }
}

static constexpr const char *DiscordOriginHeader = "Origin: https://discord.com";

static curl_slist *toCurlList(const QList<QByteArray> &headers)
{
    curl_slist *list = nullptr;
    for (const QByteArray &header : headers)
        list = curl_slist_append(list, header.constData());
    return list;
}

// chrome orders these by name, not by when they were set. from captures of fetch() with each of discord's header sets.
// md5 never met captcha/context in one capture, nor audit log reason the captcha session id
static qsizetype headerRank(const QByteArray &header)
{
    static const QList<QByteArray> order = { "x-captcha-rqtoken", "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-captcha-key", "x-context-properties", "x-discord-original-md5", "x-super-properties", "x-captcha-session-id", "x-audit-log-reason", "x-discord-locale", "user-agent", "content-type", "x-fingerprint" };
    // "Name;" is curl's empty header
    const QByteArray name = header.split(':').first().split(';').first().toLower();
    const qsizetype rank = order.indexOf(name);
    return rank < 0 ? order.size() : rank;
}

curl_slist *browserHeaders(const BrowserRequest &request)
{
    QList<QByteArray> ranked = request.pageHeaders;
    ranked.append("sec-ch-ua-platform: \"Windows\"");
    ranked.append("sec-ch-ua: " + getSecChUa());
    ranked.append("sec-ch-ua-mobile: ?0");
    ranked.append("User-Agent: " + getUserAgent().toUtf8());
    if (!request.contentType.isEmpty())
        ranked.append("Content-Type: " + request.contentType);
    std::stable_sort(ranked.begin(), ranked.end(), [](const QByteArray &a, const QByteArray &b) { return headerRank(a) < headerRank(b); });

    QList<QByteArray> headers;
    if (request.contentLength)
        headers.append("Content-Length: " + QByteArray::number(*request.contentLength));
    headers.append(ranked);
    headers.append("Accept: " + request.accept);
    if (request.sendsOrigin)
        headers.append(DiscordOriginHeader);
    headers.append(QByteArray("Sec-Fetch-Site: ") + (request.crossSite ? "cross-site" : "same-origin"));
    headers.append("Sec-Fetch-Mode: " + request.mode);
    headers.append("Sec-Fetch-Dest: " + request.dest);
    if (!request.referer.isEmpty())
        headers.append("Referer: " + request.referer);
    headers.append("Accept-Encoding: " + request.acceptEncoding);
    headers.append("Accept-Language: " + getAcceptLanguage());
    if (!request.range.isEmpty())
        headers.append("Range: " + request.range);
    if (!request.priority.isEmpty())
        headers.append("Priority: " + request.priority);
    return toCurlList(headers);
}

QList<QByteArray> discordHeaders(const ClientIdentity &identity, const QString &token, const QString &fingerprint)
{
    static const QByteArray tz = QTimeZone::systemTimeZoneId();

    ClientPropertiesBuildParams params;
    params.includeClientHeartbeatSessionId = true;
    const QByteArray superProperties = identity.buildClientProperties(params).toJson().toBytes().toBase64();

    QList<QByteArray> headers;
    if (!token.isEmpty())
        headers.append("Authorization: " + token.toUtf8());
    headers.append("X-Super-Properties: " + superProperties);
    if (!fingerprint.isEmpty())
        headers.append("X-Fingerprint: " + fingerprint.toUtf8());
    headers.append("X-Discord-Locale: " + identity.discordLocale().toUtf8());
    headers.append("X-Discord-Timezone: " + tz);
    headers.append("X-Debug-Options: bugReporterEnabled");
    return headers;
}

// curl can't inflate permessage-deflate frames
static std::mutex compressingHostsMutex;
static QSet<QString> compressingHosts;

static bool offersCompressionTo(const QString &host)
{
    std::lock_guard lock(compressingHostsMutex);
    return !compressingHosts.contains(host);
}

static QByteArray randomWebSocketKey()
{
    QByteArray nonce(16, Qt::Uninitialized);
    QRandomGenerator::system()->generate(nonce.begin(), nonce.end());
    return nonce.toBase64();
}

static curl_slist *webSocketHeaders(const QString &host)
{
    QList<QByteArray> headers = {
        "Pragma: no-cache",
        "Cache-Control: no-cache",
        "User-Agent: " + getUserAgent().toUtf8(),
        "Upgrade: websocket",
        DiscordOriginHeader,
        "Sec-WebSocket-Version: 13",
        QByteArray("Accept-Encoding: ") + ChromeAcceptEncoding,
        "Accept-Language: " + getAcceptLanguage(),
        // curl-impersonate would put its own key right after Host
        "Sec-WebSocket-Key: " + randomWebSocketKey(),
    };
    if (offersCompressionTo(host))
        headers.append("Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits");
    headers.append("Accept:"); // drops curl's default, chrome sends none on a handshake
    return toCurlList(headers);
}

CURLcode performWebSocketHandshake(CURL *curl, const QByteArray &url)
{
    const QString host = QUrl(QString::fromUtf8(url)).host();
    curl_slist *headers = webSocketHeaders(host);
    curl_easy_setopt(curl, CURLOPT_URL, url.constData());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    curl_header *extensions = nullptr;
    if (curl_easy_header(curl, "Sec-WebSocket-Extensions", 0, CURLH_HEADER | CURLH_1XX, -1, &extensions) == CURLHE_OK) {
        qCWarning(LogNetwork) << host << "accepted WebSocket compression:" << extensions->value << "- no longer offering it there";
        std::lock_guard lock(compressingHostsMutex);
        compressingHosts.insert(host);
        return res == CURLE_OK ? CURLE_UNSUPPORTED_PROTOCOL : res;
    }
    return res;
}

size_t writeToByteArray(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t bytes = size * nmemb;
    static_cast<QByteArray *>(userdata)->append(ptr, static_cast<int>(bytes));
    return bytes;
}

curl_socket_t getActiveSocket(CURL *curl)
{
    curl_socket_t sockfd = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
    return sockfd;
}

void waitOnSocket(curl_socket_t sockfd, bool forWrite, long timeoutMs)
{
    if (sockfd == CURL_SOCKET_BAD)
        return;

    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);

    if (forWrite)
        select(static_cast<int>(sockfd) + 1, nullptr, &fds, nullptr, &timeout);
    else
        select(static_cast<int>(sockfd) + 1, &fds, nullptr, nullptr, &timeout);
}

bool wsSend(CURL *&curl, std::mutex &mutex, const char *data, size_t len, unsigned flags,
            const char *what)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!curl)
        return false;

    size_t sentTotal = 0;
    while (sentTotal < len) {
        size_t sentNow = 0;
        CURLcode res = curl_ws_send(curl, data + sentTotal, len - sentTotal, &sentNow, 0, flags);

        if (res == CURLE_OK) {
            sentTotal += sentNow;
        } else if (res == CURLE_AGAIN) {
            waitOnSocket(getActiveSocket(curl), true, 100);
        } else {
            qCWarning(LogNetwork) << "Error sending" << what
                                  << "WebSocket payload:" << curl_easy_strerror(res);
            return false;
        }
    }
    return true;
}

void wsRecvWait(CURL *&curl, std::mutex &mutex)
{
    curl_socket_t sockfd;
    {
        std::lock_guard<std::mutex> lock(mutex);
        sockfd = getActiveSocket(curl);
    }
    waitOnSocket(sockfd, false, 10);
}

} // namespace CurlUtils
} // namespace Discord
} // namespace Acheron
