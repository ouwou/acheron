#include "CurlUtils.hpp"

#include "ClientIdentity.hpp"
#include "Core/Logging.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

#include <utility>
#include <vector>

namespace Acheron {
namespace Discord {
namespace CurlUtils {

static int cachedBuildNumber = 0;
static bool buildNumberFetchInFlight = false;
static std::vector<std::function<void()>> buildNumberWaiters;
static constexpr int fallbackBuildNumber = 482285;
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

    QNetworkRequest request(QUrl("https://discord.com/app"));
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

        QNetworkRequest sentryRequest(sentryUrl);
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

QString getUserAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/142.0.0.0 Safari/537.36";
}

QString getImpersonateTarget()
{
    return "chrome142";
}

UserAgentProps getUserAgentProps()
{
    return { "Windows", "Chrome", "142.0.0.0", "10" };
}

void applyCommonOptions(CURL *curl)
{
    static const QString certPath = getCertificatePath();
    if (!certPath.isEmpty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, certPath.toUtf8().constData());
#ifdef IS_CURL_IMPERSONATE
    curl_easy_impersonate(curl, getImpersonateTarget().toUtf8().constData(), 1);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, getUserAgent().toUtf8().constData());
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

void appendDiscordHeaders(curl_slist **headers, const ClientIdentity &identity, const QString &referer)
{
    static const QString tz = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    static const QString locale = QLocale::system().name();

    ClientPropertiesBuildParams params;
    params.clientAppState = "focused";
    params.includeClientHeartbeatSessionId = true;
    QString superProperties = QJsonDocument(identity.buildClientProperties(params).toJson())
                                      .toJson(QJsonDocument::Compact)
                                      .toBase64();

    *headers = curl_slist_append(*headers, ("X-Discord-Timezone: " + tz).toUtf8().constData());
    *headers = curl_slist_append(*headers, ("X-Discord-Locale: " + locale).toUtf8().constData());
    *headers = curl_slist_append(*headers, ("X-Super-Properties: " + superProperties).toUtf8().constData());
    *headers = curl_slist_append(*headers, "X-Debug-Options: bugReporterEnabled");
    *headers = curl_slist_append(*headers, ("Referer: " + referer).toUtf8().constData());
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
