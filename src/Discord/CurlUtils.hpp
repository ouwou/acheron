#pragma once

#include <QString>
#include <QObject>

#include "Core/ProxyConfig.hpp"

#include <curl/curl.h>

#include <functional>
#include <mutex>

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

void ensureBuildNumber(QNetworkAccessManager *nam, std::function<void()> done);
int getBuildNumber();

// cacert, curl-impersonate, user-agent
void applyCommonOptions(CURL *curl);

void applyProxy(CURL *curl, const Core::ProxyConfig &proxy);

void appendDiscordHeaders(curl_slist **headers, const ClientIdentity &identity, const QString &referer);

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
