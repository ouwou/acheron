#include "HttpClient.hpp"
#include "CurlUtils.hpp"
#include "ClientIdentity.hpp"

#include <curl/curl.h>
#include <QJsonDocument>
#include <QJsonObject>

#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {

static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    QByteArray *data = static_cast<QByteArray *>(userp);
    data->append(static_cast<const char *>(contents), realsize);
    return realsize;
}

HttpClient::HttpClient(const QString &baseUrl, const QString &token, ClientIdentity &identity,
                       QObject *parent)
    : QObject(parent), baseUrl(baseUrl), token(token), identity(identity)
{
    setupSharing();
}

void HttpClient::get(const QString &endpoint, const QUrlQuery &query, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    if (!query.isEmpty())
        url += "?" + query.toString(QUrl::FullyEncoded);
    executeRequest(Method::GET, url, {}, callback);
}

void HttpClient::post(const QString &endpoint, const QJsonObject &body, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    executeRequest(Method::POST, url, data, callback);
}

void HttpClient::patch(const QString &endpoint, const QJsonObject &body, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    executeRequest(Method::PATCH, url, data, callback);
}

void HttpClient::put(const QString &endpoint, const QJsonObject &body, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    executeRequest(Method::PUT, url, data, callback);
}

void HttpClient::delete_(const QString &endpoint, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    executeRequest(Method::DELETE_, url, {}, callback);
}

void HttpClient::delete_(const QString &endpoint, const QJsonObject &body, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    executeRequest(Method::DELETE_, url, data, callback);
}

void HttpClient::postMultipart(const QString &endpoint, const QJsonObject &jsonPayload,
                               const QList<FileUpload> &files, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    QByteArray jsonData = QJsonDocument(jsonPayload).toJson(QJsonDocument::Compact);
    executeMultipartRequest(url, jsonData, files, callback);
}

void HttpClient::lock_cb(CURL *handle, curl_lock_data data, curl_lock_access access, void *userptr)
{
    switch (data) {
    case CURL_LOCK_DATA_COOKIE:
        shareMutexes[0].lock();
        break;
    case CURL_LOCK_DATA_DNS:
        shareMutexes[1].lock();
        break;
    case CURL_LOCK_DATA_CONNECT:
        shareMutexes[2].lock();
        break;
    default:
        break;
    }
}

void HttpClient::unlock_cb(CURL *handle, curl_lock_data data, void *userptr)
{
    switch (data) {
    case CURL_LOCK_DATA_COOKIE:
        shareMutexes[0].unlock();
        break;
    case CURL_LOCK_DATA_DNS:
        shareMutexes[1].unlock();
        break;
    case CURL_LOCK_DATA_CONNECT:
        shareMutexes[2].unlock();
        break;
    default:
        break;
    }
}

void HttpClient::setupSharing()
{
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    // curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock_cb);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlock_cb);
}

void HttpClient::executeRequest(Method method, const QString &url, const QByteArray &data,
                                HttpCallback callback)
{
    std::string sUrl = url.toStdString();
    std::string sToken = token.toStdString();

    // i should use curl_multi but for now i will deal with the share obj interface
    // cuz its easier than dealing with multi for now
    std::thread([=, this]() {
        CURL *curl = curl_easy_init();
        HttpResponse response;

        if (curl) {
            QString certPath = CurlUtils::getCertificatePath();
            if (!certPath.isEmpty())
                curl_easy_setopt(curl, CURLOPT_CAINFO, certPath.toUtf8().constData());

#ifdef IS_CURL_IMPERSONATE
            curl_easy_impersonate(curl, CurlUtils::getImpersonateTarget().toUtf8().constData(), 1);
#endif
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                             CurlUtils::getUserAgent().toUtf8().constData());

            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, ""); // engine
            curl_easy_setopt(curl, CURLOPT_SHARE, share);

            // curl_easy_setopt(curl, CURLOPT_PROXY, "http://127.0.0.1:8888");
            // dont verify
            // curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, ("Authorization: " + sToken).c_str());
            headers = curl_slist_append(headers, "Content-Type: application/json");

            static QString tz = QString::fromUtf8(QTimeZone::systemTimeZoneId());
            static QString locale = QLocale::system().name();
            ClientPropertiesBuildParams params;
            params.clientAppState = "focused";
            params.includeClientHeartbeatSessionId = true;
            ClientProperties props = identity.buildClientProperties(params);
            QString superProperties =
                    QJsonDocument(props.toJson()).toJson(QJsonDocument::Compact).toBase64();
            // clang-format off
            curl_slist_append(headers, ("X-Discord-Timezone: " + tz).toUtf8().constData());
            curl_slist_append(headers, ("X-Discord-Locale: " + locale).toUtf8().constData());
			curl_slist_append(headers, ("X-Super-Properties: " + superProperties).toUtf8().constData());
            curl_slist_append(headers, "X-Debug-Options: bugReporterEnabled");
            // there is more logic with referer but not super important
            curl_slist_append(headers, "Referer: https://discord.com/channels/@me");
            // clang-format on

            curl_easy_setopt(curl, CURLOPT_URL, sUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            // otherwise body is compressed for some reason
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

            switch (method) {
            case Method::GET:
                break;
            case Method::POST:
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.constData());
                break;
            case Method::PATCH:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.constData());
                break;
            case Method::PUT:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.constData());
                break;
            case Method::DELETE_:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                if (!data.isEmpty()) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.constData());
                }
                break;
            }

            CURLcode res = curl_easy_perform(curl);

            if (res != CURLE_OK) {
                qCWarning(LogNetwork) << "HTTP error: " << curl_easy_strerror(res);
                response.error = curl_easy_strerror(res);
                response.success = false;
            } else {
                long httpCode = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                response.statusCode = static_cast<int>(httpCode);
                response.success = (httpCode >= 200 && httpCode < 300);
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        } else {
            qCCritical(LogNetwork) << "Failed to initialize curl";
            response.error = "Failed to initialize curl";
            response.success = false;
        }

        QMetaObject::invokeMethod(qApp, [callback, response]() { callback(response); });
    }).detach();
}

void HttpClient::executeMultipartRequest(const QString &url, const QByteArray &jsonData,
                                         const QList<FileUpload> &files, HttpCallback callback)
{
    std::string sUrl = url.toStdString();
    std::string sToken = token.toStdString();

    std::thread([=, this]() {
        CURL *curl = curl_easy_init();
        HttpResponse response;


        if (curl) {
            QString certPath = CurlUtils::getCertificatePath();
            if (!certPath.isEmpty())
                curl_easy_setopt(curl, CURLOPT_CAINFO, certPath.toUtf8().constData());

#ifdef IS_CURL_IMPERSONATE
            curl_easy_impersonate(curl, CurlUtils::getImpersonateTarget().toUtf8().constData(), 1);
#endif
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                             CurlUtils::getUserAgent().toUtf8().constData());
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, ""); // engine
            curl_easy_setopt(curl, CURLOPT_SHARE, share);

            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, ("Authorization: " + sToken).c_str());
            // Do NOT set Content-Type — curl_mime sets multipart/form-data automatically

            static QString tz = QString::fromUtf8(QTimeZone::systemTimeZoneId());
            static QString locale = QLocale::system().name();
            ClientPropertiesBuildParams params;
            params.clientAppState = "focused";
            params.includeClientHeartbeatSessionId = true;
            ClientProperties props = identity.buildClientProperties(params);
            QString superProperties =
                    QJsonDocument(props.toJson()).toJson(QJsonDocument::Compact).toBase64();
            // clang-format off
            curl_slist_append(headers, ("X-Discord-Timezone: " + tz).toUtf8().constData());
            curl_slist_append(headers, ("X-Discord-Locale: " + locale).toUtf8().constData());
			curl_slist_append(headers, ("X-Super-Properties: " + superProperties).toUtf8().constData());
            curl_slist_append(headers, "X-Debug-Options: bugReporterEnabled");
            curl_slist_append(headers, "Referer: https://discord.com/channels/@me");
            // clang-format on

            curl_easy_setopt(curl, CURLOPT_URL, sUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

            curl_mime *mime = curl_mime_init(curl);

            // payload_json part
            curl_mimepart *jsonPart = curl_mime_addpart(mime);
            curl_mime_name(jsonPart, "payload_json");
            curl_mime_data(jsonPart, jsonData.constData(), jsonData.size());
            curl_mime_type(jsonPart, "application/json");

            // file parts
            for (int i = 0; i < files.size(); i++) {
                curl_mimepart *filePart = curl_mime_addpart(mime);
                curl_mime_name(filePart, QString("files[%1]").arg(i).toUtf8().constData());
                curl_mime_data(filePart, files[i].data.constData(), files[i].data.size());
                curl_mime_filename(filePart, files[i].filename.toUtf8().constData());
                curl_mime_type(filePart, files[i].mimeType.toUtf8().constData());
            }

            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

            CURLcode res = curl_easy_perform(curl);

            if (res != CURLE_OK) {
                qCWarning(LogNetwork) << "HTTP multipart error: " << curl_easy_strerror(res);
                response.error = curl_easy_strerror(res);
                response.success = false;
            } else {
                long httpCode = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                response.statusCode = static_cast<int>(httpCode);
                response.success = (httpCode >= 200 && httpCode < 300);
            }

            curl_mime_free(mime);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        } else {
            qCCritical(LogNetwork) << "Failed to initialize curl";
            response.error = "Failed to initialize curl";
            response.success = false;
        }

        QMetaObject::invokeMethod(qApp, [callback, response]() { callback(response); });
    }).detach();
}

} // namespace Discord
} // namespace Acheron
