#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QObject>

#include "Core/ProxyConfig.hpp"

#include <curl/curl.h>

#include <functional>
#include <mutex>
#include <optional>

class QNetworkAccessManager;

namespace Acheron {
namespace Discord {

class ClientIdentity;

namespace CurlUtils {

struct UserAgentProps
{
    QString os;
    QString browser;
    QString browserVersion;
    QString osVersion;
};

QString getCertificatePath();
QString getUserAgent();
QString getImpersonateTarget();
UserAgentProps getUserAgentProps();
QString getSystemLocale();
QByteArray getAcceptLanguage();

void ensureBuildNumber(QNetworkAccessManager *nam, std::function<void()> done);
int getBuildNumber();

// cacert, curl-impersonate tls/h2 (no default headers)
void applyCommonOptions(CURL *curl);

void applyProxy(CURL *curl, const Core::ProxyConfig &proxy);

inline constexpr const char *ChromeAcceptEncoding = "gzip, deflate, br, zstd";

// a request chrome makes from a discord.com page
struct BrowserRequest
{
    QList<QByteArray> pageHeaders; // set by discord's js
    std::optional<qint64> contentLength;
    QByteArray contentType;
    QByteArray accept = "*/*";
    bool sendsOrigin = false;
    bool crossSite = false;
    QByteArray mode = "cors";
    QByteArray dest = "empty";
    QByteArray referer;
    QByteArray acceptEncoding = ChromeAcceptEncoding;
    QByteArray range;
    QByteArray priority = "u=1, i";
};

curl_slist *browserHeaders(const BrowserRequest &request);

// what discord's api layer adds to every request
QList<QByteArray> discordHeaders(const ClientIdentity &identity, const QString &token, const QString &fingerprint);

// handshake for a CURLOPT_CONNECT_ONLY=2 websocket handle
CURLcode performWebSocketHandshake(CURL *curl, const QByteArray &url);

// CURLOPT_WRITEFUNCTION for QByteArray
size_t writeToByteArray(char *ptr, size_t size, size_t nmemb, void *userdata);

// send full frame with retry
bool wsSend(CURL *&curl, std::mutex &mutex, const char *data, size_t len, unsigned flags, const char *what);

// shortcut for waiting on the socket
void wsRecvWait(CURL *&curl, std::mutex &mutex);

curl_socket_t getActiveSocket(CURL *curl);

void waitOnSocket(curl_socket_t sockfd, bool forWrite, long timeoutMs);

} // namespace CurlUtils
} // namespace Discord
} // namespace Acheron
