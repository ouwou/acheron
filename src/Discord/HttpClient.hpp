#pragma once

#include <QObject>

#include <curl/curl.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "CaptchaResolver.hpp"
#include "Core/ProxyConfig.hpp"

namespace Acheron {
namespace Discord {

class ClientIdentity;
class RequestWorker;
struct RequestDescriptor;

struct HttpResponse
{
    int statusCode = 0;
    QByteArray body;
    QString error;
    bool success = false;

    [[nodiscard]] bool rateLimited() const { return statusCode == 429; }
};

struct FileUpload
{
    QString filename;
    QByteArray data;
    QString mimeType;
};

using HttpCallback = std::function<void(const HttpResponse &)>;

class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(const QString &baseUrl, const QString &token, ClientIdentity &identity,
                        const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr,
                        QObject *parent = nullptr);
    ~HttpClient() override;

    enum class Method {
        GET,
        POST,
        PUT,
        PATCH,
        DELETE_, // thanks windows.h for the DELETE macro
    };

    void get(const QString &endpoint, const QUrlQuery &query, HttpCallback callback);
    void post(const QString &endpoint, const QJsonObject &body, HttpCallback callback);
    void patch(const QString &endpoint, const QJsonObject &body, HttpCallback callback);
    void put(const QString &endpoint, const QJsonObject &body, HttpCallback callback);
    void delete_(const QString &endpoint, HttpCallback callback);
    void delete_(const QString &endpoint, const QJsonObject &body, HttpCallback callback);
    void postMultipart(const QString &endpoint, const QJsonObject &jsonPayload,
                       const QList<FileUpload> &files, HttpCallback callback);
    // for gcp uploads
    void putExternalFile(const QString &absoluteUrl, const QString &filePath,
                         const QString &contentType, HttpCallback callback,
                         std::function<void(qint64 sent, qint64 total)> progress = {},
                         std::shared_ptr<std::atomic<bool>> cancelFlag = {});
    void putExternal(const QString &absoluteUrl, const QByteArray &data,
                     const QString &contentType, HttpCallback callback,
                     std::function<void(qint64 sent, qint64 total)> progress = {},
                     std::shared_ptr<std::atomic<bool>> cancelFlag = {});

private:
    void executeRequest(Method method, const QString &url, const QByteArray &data,
                        HttpCallback callback);
    void executeMultipartRequest(const QString &url, const QByteArray &jsonData,
                                 const QList<FileUpload> &files, HttpCallback callback);
    void submitExternalPut(RequestDescriptor &descriptor,
                           std::function<void(qint64, qint64)> progress);

    void onRequestComplete(RequestDescriptor descriptor, HttpResponse response,
                           std::optional<CaptchaChallenge> challenge);

    QString baseUrl;
    QString token;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver;

    std::unique_ptr<RequestWorker> worker;

    friend class RequestWorker;
};

} // namespace Discord
} // namespace Acheron
