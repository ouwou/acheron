#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

#include <curl/curl.h>

#include "CaptchaResolver.hpp"
#include "Core/ProxyConfig.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

struct evp_pkey_st;
struct evp_pkey_ctx_st;

class QJsonObject;

namespace Acheron {
namespace Discord {

enum class RemoteAuthError {
    KeyGenerationFailed,
    ConnectionFailed,
    HandshakeFailed,
    TimedOut,
    CaptchaRequired,
    LoginFailed,
};

class RemoteAuthClient : public QObject
{
    Q_OBJECT
public:
    explicit RemoteAuthClient(const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr, QObject *parent = nullptr);
    ~RemoteAuthClient();

    void start();
    void stop();

signals:
    void fingerprintReady(const QString &fingerprint);
    void pendingTicket(const QString &userId, const QString &username, const QString &avatar);
    void authenticated(const QString &token);
    void failed(RemoteAuthError error);
    void canceled();

private:
    bool generateKey();
    void freeKey();
    QByteArray publicKeyDer() const;
    QByteArray decrypt(const QByteArray &in) const;

    void networkLoop();
    void heartbeatLoop();

    void handleMessage(const QString &text);
    void handleHello(const QJsonObject &obj);
    void handleNonceProof(const QJsonObject &obj);
    void handlePendingRemoteInit(const QJsonObject &obj);
    void handlePendingTicket(const QJsonObject &obj);
    void handlePendingLogin(const QJsonObject &obj);
    void postLogin(const QString &ticket, std::optional<CaptchaSolution> solution, int attempt);

    void send(const QJsonObject &obj);

    void fail(RemoteAuthError error);
    void succeed(const QString &token);
    void cancel();

    evp_pkey_st *pkey = nullptr;
    evp_pkey_ctx_st *keyCtx = nullptr;
    evp_pkey_ctx_st *decCtx = nullptr;

    std::atomic<bool> running{ false };
    std::atomic<bool> done{ false };

    std::mutex curlMutex;
    CURL *curl = nullptr;
    std::thread networkThread;

    std::atomic<int> heartbeatInterval{ 0 };
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;

    std::thread httpThread;

    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver = nullptr;
};

} // namespace Discord
} // namespace Acheron
