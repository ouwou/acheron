#include "RequestWorker.hpp"

#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"
#include "HttpClient.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <utility>

#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {

static size_t readCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
    auto *file = static_cast<QFile *>(userp);
    qint64 read = file->read(buffer, static_cast<qint64>(size) * nitems);
    if (read < 0)
        return CURL_READFUNC_ABORT;
    return static_cast<size_t>(read);
}

static int seekCallback(void *userp, curl_off_t offset, int origin)
{
    auto *file = static_cast<QFile *>(userp);
    if (origin != SEEK_SET)
        return CURL_SEEKFUNC_CANTSEEK;
    return file->seek(offset) ? CURL_SEEKFUNC_OK : CURL_SEEKFUNC_FAIL;
}

static int xferinfoCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    Q_UNUSED(dltotal);
    Q_UNUSED(dlnow);
    auto *ctx = static_cast<TransferContext *>(clientp);
    const RequestDescriptor &desc = ctx->descriptor;
    if (desc.cancelFlag && desc.cancelFlag->load())
        return 1; // abort
    // slow tf down
    constexpr curl_off_t minDelta = 256 * 1024;
    bool finished = ultotal > 0 && ulnow == ultotal;
    if (desc.progressCallback && ulnow != ctx->lastProgressSent &&
        (finished || ulnow - ctx->lastProgressSent >= minDelta)) {
        ctx->lastProgressSent = ulnow;
        desc.progressCallback(static_cast<qint64>(ulnow), static_cast<qint64>(ultotal));
    }
    return 0;
}

RequestWorker::RequestWorker(HttpClient *owner, QString token, ClientIdentity &identity,
                             const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver)
    : owner(owner), ownerGuard(owner), token(std::move(token)), identity(identity), proxy(proxy), captchaResolver(captchaResolver)
{
    multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));

    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);

    running.store(true);
    thread = std::thread(&RequestWorker::threadLoop, this);
}

RequestWorker::~RequestWorker()
{
    shutdown();
}

void RequestWorker::submit(RequestDescriptor descriptor)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!acceptingSubmissions)
        return;
    pending.push_back(std::move(descriptor));
    // multi is always valid here by virtue of mutex w/ shutdown
    curl_multi_wakeup(multi);
}

void RequestWorker::shutdown()
{
    if (!thread.joinable())
        return; // already shut down

    {
        std::lock_guard<std::mutex> lock(mutex);
        acceptingSubmissions = false;
    }
    running.store(false);
    curl_multi_wakeup(multi);
    thread.join();

    // worker is dead. safe to touch
    if (multi) {
        curl_multi_cleanup(multi);
        multi = nullptr;
    }
    if (share) {
        curl_share_cleanup(share);
        share = nullptr;
    }
}

void RequestWorker::threadLoop()
{
    while (running.load()) {
        drainSubmissions();

        int stillRunning = 0;
        curl_multi_perform(multi, &stillRunning);

        int msgsLeft = 0;
        CURLMsg *msg = nullptr;
        while ((msg = curl_multi_info_read(multi, &msgsLeft))) {
            if (msg->msg != CURLMSG_DONE)
                continue;

            CURL *easy = msg->easy_handle;
            CURLcode result = msg->data.result;
            TransferContext *ctx = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ctx);
            if (ctx)
                handleCompletion(ctx, result);
        }

        startReleased();

        if (!running.load())
            break;

        int numfds = 0;
        curl_multi_poll(multi, nullptr, 0, pollTimeoutMs(), &numfds);
    }

    abortAllInFlight();
}

void RequestWorker::drainSubmissions()
{
    std::deque<RequestDescriptor> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        batch.swap(pending);
    }

    for (auto &descriptor : batch)
        admit(std::move(descriptor));
}

static std::string rateLimitKey(const RequestDescriptor &descriptor)
{
    return descriptor.url.substr(0, descriptor.url.find('?'));
}

void RequestWorker::admit(RequestDescriptor descriptor)
{
    if (descriptor.external) {
        startTransfer(std::move(descriptor));
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (limiter.refusing(now)) {
        refuse(std::move(descriptor));
        return;
    }
    refusingLogged = false;

    const std::string key = rateLimitKey(descriptor);
    if (auto admitted = limiter.admit(key, std::move(descriptor), now))
        startTransfer(std::move(*admitted));
}

void RequestWorker::startReleased()
{
    for (auto &descriptor : limiter.takeReleased(std::chrono::steady_clock::now()))
        startTransfer(std::move(descriptor));
}

long RequestWorker::pollTimeoutMs() const
{
    constexpr long idleMs = 1000;
    const auto wake = limiter.nextWake();
    if (!wake)
        return idleMs;
    const auto untilWake = std::chrono::duration_cast<std::chrono::milliseconds>(*wake - std::chrono::steady_clock::now()).count();
    return std::clamp<long>(static_cast<long>(untilWake) + 1, 0, idleMs);
}

void RequestWorker::startTransfer(RequestDescriptor descriptor)
{
    auto *ctx = new TransferContext();
    ctx->worker = this;
    ctx->descriptor = std::move(descriptor);

    CURL *easy = buildEasyHandle(ctx);
    if (!easy) {
        failTransfer(ctx, "Failed to initialize curl");
        return;
    }

    CURLMcode mc = curl_multi_add_handle(multi, easy);
    if (mc != CURLM_OK) {
        qCWarning(LogNetwork) << "curl_multi_add_handle failed:" << curl_multi_strerror(mc);
        failTransfer(ctx, "Failed to add transfer");
        return;
    }

    active.insert(ctx);
}

void RequestWorker::refuse(RequestDescriptor descriptor)
{
    if (!refusingLogged) {
        refusingLogged = true;
        qCWarning(LogNetwork) << "Discord refused" << RateLimiter<RequestDescriptor>::maxRefusals << "requests in 10 minutes, not sending any more for now";
    }
    HttpResponse response;
    response.error = "Too many refused requests";
    dispatchCompletion(std::move(descriptor), std::move(response), std::nullopt);
}

static std::optional<QByteArray> responseHeader(CURL *easy, const char *name)
{
    curl_header *header = nullptr;
    if (curl_easy_header(easy, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK)
        return std::nullopt;
    return QByteArray(header->value);
}

static RateLimitResponse rateLimitResponse(CURL *easy, const HttpResponse &response)
{
    RateLimitResponse limit;
    limit.status = response.statusCode;
    limit.retryAfterMs = response.retryAfterMs;

    if (limit.status == 429) {
        const QJsonObject body = QJsonDocument::fromJson(response.body).object();
        limit.global = body.value("global").toBool();
        const double retryAfter = body.value("retry_after").toDouble();
        if (!limit.retryAfterMs && retryAfter > 0)
            limit.retryAfterMs = static_cast<int>(retryAfter * 1000);
    }

    char *contentType = nullptr;
    curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &contentType);
    const bool json = contentType && QByteArray(contentType).startsWith("application/json");
    limit.blockedByCloudflare = (limit.status == 403 || limit.status == 429) && !json;

    bool ok = false;
    if (const auto remaining = responseHeader(easy, "x-ratelimit-remaining")) {
        const int value = remaining->toInt(&ok);
        if (ok)
            limit.bucketRemaining = value;
    }
    if (const auto resetAfter = responseHeader(easy, "x-ratelimit-reset-after")) {
        const double seconds = resetAfter->toDouble(&ok);
        if (ok)
            limit.bucketResetAfterMs = static_cast<int>(seconds * 1000);
    }
    return limit;
}

CURL *RequestWorker::buildEasyHandle(TransferContext *ctx)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        qCCritical(LogNetwork) << "Failed to initialize curl";
        return nullptr;
    }

    const RequestDescriptor &desc = ctx->descriptor;

    if (!desc.uploadFilePath.isEmpty()) {
        ctx->uploadFile = new QFile(desc.uploadFilePath);
        if (!ctx->uploadFile->open(QIODevice::ReadOnly)) {
            qCWarning(LogNetwork) << "Failed to open upload file:" << desc.uploadFilePath;
            curl_easy_cleanup(curl);
            return nullptr;
        }
    }

    CurlUtils::applyCommonOptions(curl);
    CurlUtils::applyProxy(curl, proxy);

    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, ""); // engine
    curl_easy_setopt(curl, CURLOPT_SHARE, share);

    CurlUtils::BrowserRequest browser;
    browser.sendsOrigin = desc.method != HttpClient::Method::GET;
    if (!desc.multipart && !ctx->uploadFile && desc.method != HttpClient::Method::GET && (desc.method != HttpClient::Method::DELETE_ || !desc.body.isEmpty()))
        browser.contentLength = desc.body.size();

    if (desc.external) {
        browser.crossSite = true;
        browser.referer = "https://discord.com/";
        browser.contentType = desc.contentType.toUtf8();
        if (ctx->uploadFile)
            browser.contentLength = ctx->uploadFile->size();
    } else {
        if (!desc.multipart && !desc.body.isEmpty())
            browser.contentType = "application/json";
        browser.referer = desc.referer.toUtf8();

        if (!desc.originalMd5.isEmpty())
            browser.pageHeaders.append("X-Discord-Original-MD5: " + desc.originalMd5);
        if (desc.auditLogReason) {
            const QByteArray reason = encodeUriComponent(*desc.auditLogReason);
            browser.pageHeaders.append(reason.isEmpty() ? QByteArray("X-Audit-Log-Reason;") : "X-Audit-Log-Reason: " + reason);
        }
        if (!desc.contextProperties.isEmpty())
            browser.pageHeaders.append("X-Context-Properties: " + desc.contextProperties);
        if (desc.solution)
            browser.pageHeaders.append(captchaHeaders(*desc.solution));
        browser.pageHeaders.append(CurlUtils::discordHeaders(identity, token, desc.fingerprint));
    }

    curl_slist *headers = CurlUtils::browserHeaders(browser);
    if (ctx->uploadFile)
        headers = curl_slist_append(headers, "Expect:"); // CURLOPT_UPLOAD turns this on

    ctx->headers = headers;
    ctx->url = desc.url;

    curl_easy_setopt(curl, CURLOPT_URL, ctx->url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // otherwise body is compressed for some reason
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlUtils::writeToByteArray);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx->response.body);

    if (desc.multipart) {
        curl_mime *mime = curl_mime_init(curl);

        if (desc.formFields.isEmpty()) {
            curl_mimepart *jsonPart = curl_mime_addpart(mime);
            curl_mime_name(jsonPart, "payload_json");
            curl_mime_data(jsonPart, desc.body.constData(), desc.body.size());
            curl_mime_type(jsonPart, "application/json");
        }

        for (const auto &field : desc.formFields) {
            curl_mimepart *fieldPart = curl_mime_addpart(mime);
            const QByteArray value = field.second.toUtf8();
            curl_mime_name(fieldPart, field.first.toUtf8().constData());
            curl_mime_data(fieldPart, value.constData(), value.size());
        }

        // file parts
        for (int i = 0; i < desc.files.size(); i++) {
            curl_mimepart *filePart = curl_mime_addpart(mime);
            const QString partName = desc.files[i].fieldName.isEmpty() ? QString("files[%1]").arg(i) : desc.files[i].fieldName;
            curl_mime_name(filePart, partName.toUtf8().constData());
            curl_mime_data(filePart, desc.files[i].data.constData(), desc.files[i].data.size());
            curl_mime_filename(filePart, desc.files[i].filename.toUtf8().constData());
            curl_mime_type(filePart, desc.files[i].mimeType.toUtf8().constData());
        }

        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        ctx->mime = mime;
    } else {
        switch (desc.method) {
        case HttpClient::Method::GET:
            break;
        case HttpClient::Method::POST:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, desc.body.size());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, desc.body.constData());
            break;
        case HttpClient::Method::PATCH:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, desc.body.size());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, desc.body.constData());
            break;
        case HttpClient::Method::PUT:
            if (ctx->uploadFile) {
                curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L); // marks as PUT
                curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
                curl_easy_setopt(curl, CURLOPT_READDATA, ctx->uploadFile);
                curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, seekCallback);
                curl_easy_setopt(curl, CURLOPT_SEEKDATA, ctx->uploadFile);
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(ctx->uploadFile->size()));
            } else {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, desc.body.size());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, desc.body.constData());
            }
            break;
        case HttpClient::Method::DELETE_:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (!desc.body.isEmpty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, desc.body.size());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, desc.body.constData());
            }
            break;
        }
    }

    if (desc.progressCallback || desc.cancelFlag) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfoCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
    }

    curl_easy_setopt(curl, CURLOPT_PRIVATE, ctx);
    ctx->easy = curl;
    return curl;
}

void RequestWorker::handleCompletion(TransferContext *ctx, CURLcode result)
{
    if (result == CURLE_ABORTED_BY_CALLBACK) {
        ctx->response.error = "cancelled";
        ctx->response.success = false;
    } else if (result != CURLE_OK) {
        qCWarning(LogNetwork) << "HTTP error: " << curl_easy_strerror(result);
        ctx->response.error = curl_easy_strerror(result);
        ctx->response.success = false;
    } else {
        long httpCode = 0;
        curl_easy_getinfo(ctx->easy, CURLINFO_RESPONSE_CODE, &httpCode);
        ctx->response.statusCode = static_cast<int>(httpCode);
        ctx->response.success = (httpCode >= 200 && httpCode < 300);

        // the official client only reads whole seconds here
        bool ok = false;
        if (const auto retryAfter = responseHeader(ctx->easy, "retry-after")) {
            const int seconds = retryAfter->toInt(&ok);
            if (ok && seconds > 0)
                ctx->response.retryAfterMs = seconds * 1000;
        }
    }

    if (!ctx->descriptor.external) {
        const RateLimitResponse limit = result == CURLE_OK ? rateLimitResponse(ctx->easy, ctx->response) : RateLimitResponse{};
        if (limit.blockedByCloudflare)
            qCWarning(LogNetwork) << "Blocked by Cloudflare with status" << limit.status << ", holding every request";
        else if (limit.global)
            qCWarning(LogNetwork) << "Global rate limit, holding every request for" << limit.retryAfterMs.value_or(0) << "ms";
        limiter.record(rateLimitKey(ctx->descriptor), limit, std::chrono::steady_clock::now());
    }

    std::optional<CaptchaChallenge> challenge;
    if (ctx->response.statusCode == 400 && !ctx->descriptor.external)
        challenge = CaptchaChallenge::fromResponseBody(ctx->response.body);

    // close handles b4 callbacks
    delete ctx->uploadFile;
    ctx->uploadFile = nullptr;

    dispatchCompletion(std::move(ctx->descriptor), std::move(ctx->response), std::move(challenge));

    curl_multi_remove_handle(multi, ctx->easy);
    curl_easy_cleanup(ctx->easy);
    if (ctx->mime)
        curl_mime_free(ctx->mime);
    curl_slist_free_all(ctx->headers);
    active.erase(ctx);
    delete ctx;
}

void RequestWorker::failTransfer(TransferContext *ctx, const char *error)
{
    if (ctx->mime)
        curl_mime_free(ctx->mime);
    if (ctx->headers)
        curl_slist_free_all(ctx->headers);
    if (ctx->easy)
        curl_easy_cleanup(ctx->easy);

    ctx->response.success = false;
    ctx->response.statusCode = 0;
    ctx->response.error = error;

    dispatchCompletion(std::move(ctx->descriptor), std::move(ctx->response), std::nullopt);
    delete ctx;
}

void RequestWorker::dispatchCompletion(RequestDescriptor descriptor, HttpResponse response,
                                       std::optional<CaptchaChallenge> challenge)
{
    QPointer<HttpClient> guard = ownerGuard;
    QMetaObject::invokeMethod(owner, [guard, descriptor = std::move(descriptor),
                                      response = std::move(response),
                                      challenge = std::move(challenge)]() mutable {
        if (!guard)
            return;
        guard->onRequestComplete(std::move(descriptor), std::move(response), std::move(challenge));
    });
}

void RequestWorker::abortAllInFlight()
{
    for (TransferContext *ctx : active) {
        curl_multi_remove_handle(multi, ctx->easy);
        curl_easy_cleanup(ctx->easy);
        if (ctx->mime)
            curl_mime_free(ctx->mime);
        curl_slist_free_all(ctx->headers);
        delete ctx; // no callback fired
    }
    active.clear();
    limiter.clear();

    std::lock_guard<std::mutex> lock(mutex);
    pending.clear();
}

} // namespace Discord
} // namespace Acheron
