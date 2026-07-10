#include "HttpClient.hpp"
#include "RequestWorker.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrlQuery>

namespace Acheron {
namespace Discord {

HttpClient::HttpClient(const QString &baseUrl, const QString &token, ClientIdentity &identity,
                       CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), baseUrl(baseUrl), token(token), identity(identity), captchaResolver(captchaResolver)
{
    worker = std::make_unique<RequestWorker>(this, token, identity, captchaResolver);
}

HttpClient::~HttpClient()
{
    if (worker)
        worker->shutdown();
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

void HttpClient::putExternalFile(const QString &absoluteUrl, const QString &filePath,
                                 const QString &contentType, HttpCallback callback,
                                 std::function<void(qint64, qint64)> progress,
                                 std::shared_ptr<std::atomic<bool>> cancelFlag)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::PUT;
    descriptor.url = absoluteUrl.toStdString();
    descriptor.uploadFilePath = filePath;
    descriptor.external = true;
    descriptor.contentType = contentType;
    descriptor.callback = std::move(callback);
    descriptor.cancelFlag = std::move(cancelFlag);
    submitExternalPut(descriptor, std::move(progress));
}

void HttpClient::putExternal(const QString &absoluteUrl, const QByteArray &data,
                             const QString &contentType, HttpCallback callback,
                             std::function<void(qint64, qint64)> progress,
                             std::shared_ptr<std::atomic<bool>> cancelFlag)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::PUT;
    descriptor.url = absoluteUrl.toStdString();
    descriptor.body = data;
    descriptor.external = true;
    descriptor.contentType = contentType;
    descriptor.callback = std::move(callback);
    descriptor.cancelFlag = std::move(cancelFlag);
    submitExternalPut(descriptor, std::move(progress));
}

void HttpClient::submitExternalPut(RequestDescriptor &descriptor,
                                   std::function<void(qint64, qint64)> progress)
{
    if (progress) {
        QPointer<HttpClient> guard(this);
        descriptor.progressCallback = [this, guard,
                                       progress = std::move(progress)](qint64 sent, qint64 total) {
            QMetaObject::invokeMethod(this, [guard, progress, sent, total]() {
                if (guard)
                    progress(sent, total);
            });
        };
    }
    worker->submit(std::move(descriptor));
}

void HttpClient::executeRequest(Method method, const QString &url, const QByteArray &data,
                                HttpCallback callback)
{
    RequestDescriptor descriptor;
    descriptor.method = method;
    descriptor.url = url.toStdString();
    descriptor.body = data;
    descriptor.multipart = false;
    descriptor.callback = std::move(callback);
    worker->submit(std::move(descriptor));
}

void HttpClient::executeMultipartRequest(const QString &url, const QByteArray &jsonData,
                                         const QList<FileUpload> &files, HttpCallback callback)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::POST;
    descriptor.url = url.toStdString();
    descriptor.body = jsonData;
    descriptor.multipart = true;
    descriptor.files = files;
    descriptor.callback = std::move(callback);
    worker->submit(std::move(descriptor));
}

void HttpClient::onRequestComplete(RequestDescriptor descriptor, HttpResponse response,
                                   std::optional<CaptchaChallenge> challenge)
{
    if (!challenge || descriptor.captchaAttempt >= kMaxCaptchaAttempts || !captchaResolver) {
        if (captchaResolver && descriptor.captchaAttempt > 0)
            captchaResolver->notifyConcluded();
        descriptor.callback(response);
        return;
    }

    QPointer<HttpClient> self(this);
    captchaResolver->resolve(*challenge, [this, self,
                                          descriptor = std::move(descriptor),
                                          response = std::move(response)](std::optional<CaptchaSolution> s) mutable {
        if (!self)
            return;
        if (!s) {
            HttpResponse err = response;
            err.success = false;
            if (err.error.isEmpty())
                err.error = "Captcha canceled";
            descriptor.callback(err);
            return;
        }
        descriptor.captchaAttempt += 1;
        descriptor.solution = std::move(s);
        worker->submit(std::move(descriptor));
    });
}

} // namespace Discord
} // namespace Acheron
