#include "HttpClient.hpp"

#include <curl/curl.h>

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

HttpClient::HttpClient(const QString &baseUrl, const QString &token, QObject *parent)
    : QObject(parent), baseUrl(baseUrl), token(token)
{
}

void HttpClient::get(const QString &endpoint, const QUrlQuery &query, HttpCallback callback)
{
    QString url = baseUrl + endpoint;
    if (!query.isEmpty())
        url += "?" + query.toString(QUrl::FullyEncoded);
    executeRequest(Method::GET, url, {}, callback);
}

void HttpClient::executeRequest(Method method, const QString &url, const QByteArray &data,
                                HttpCallback callback)
{
    std::string sUrl = url.toStdString();
    std::string sToken = token.toStdString();

    // todo should i use qconcurrent
    std::thread([=]() {
        CURL *curl = curl_easy_init();
        HttpResponse response;

        if (curl) {
            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, ("Authorization: " + sToken).c_str());
            headers = curl_slist_append(headers, "Content-Type: application/json");

            curl_easy_setopt(curl, CURLOPT_URL, sUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
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
            default:
                qCWarning(LogNetwork) << "Unsupported method";
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

} // namespace Discord
} // namespace Acheron
