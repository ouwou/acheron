#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QPointer>
#include <QString>

#include <curl/curl.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include "CaptchaResolver.hpp"
#include "Core/ProxyConfig.hpp"
#include "HttpClient.hpp"

namespace Acheron {
namespace Discord {

class ClientIdentity;
class RequestWorker;

struct RequestDescriptor
{
    HttpClient::Method method = HttpClient::Method::GET;
    std::string url;
    QByteArray body; // json body or payload_json for multipart
    bool multipart = false;
    bool external = false; // no headers (gcp)
    QString contentType; // external Content-Type
    QString uploadFilePath; // stream from disk
    QList<FileUpload> files; // multipart
    HttpCallback callback;
    std::function<void(qint64 sent, qint64 total)> progressCallback; // worker thread!
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    int captchaAttempt = 0;
    std::optional<CaptchaSolution> solution;
};

struct TransferContext
{
    CURL *easy = nullptr;
    curl_slist *headers = nullptr;
    curl_mime *mime = nullptr; // multipart
    QFile *uploadFile = nullptr; // streamed body, worker thread only
    std::string url;
    HttpResponse response;
    RequestDescriptor descriptor;
    RequestWorker *worker = nullptr;
    qint64 lastProgressSent = -1;

    ~TransferContext() { delete uploadFile; }
};

class RequestWorker
{
public:
    RequestWorker(HttpClient *owner, QString token, ClientIdentity &identity,
                  const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver);
    ~RequestWorker();

    void submit(RequestDescriptor descriptor);

    void shutdown();

private:
    void threadLoop();
    void drainSubmissions();
    CURL *buildEasyHandle(TransferContext *ctx);
    void handleCompletion(TransferContext *ctx, CURLcode result);
    void failTransfer(TransferContext *ctx, const char *error);
    void dispatchCompletion(RequestDescriptor descriptor, HttpResponse response,
                            std::optional<CaptchaChallenge> challenge);
    void abortAllInFlight();

    HttpClient *owner;
    QPointer<HttpClient> ownerGuard; // null-check
    QString token;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver;

    CURLM *multi = nullptr;
    CURLSH *share = nullptr;

    std::thread thread;
    std::atomic<bool> running{ false };

    std::mutex mutex;
    std::deque<RequestDescriptor> pending;
    bool acceptingSubmissions = true;

    std::unordered_set<TransferContext *> active; // worker thread only
};

} // namespace Discord
} // namespace Acheron
