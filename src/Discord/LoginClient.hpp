#pragma once

#include <QObject>
#include <QString>

#include <curl/curl.h>

#include "CaptchaResolver.hpp"
#include "Core/ProxyConfig.hpp"

#include <atomic>
#include <optional>
#include <thread>

class QByteArray;
class QJsonObject;

namespace Acheron {
namespace Discord {

enum class LoginError {
    Network,
    InvalidCredentials,
    CaptchaRequired,
    InvalidMfaCode,
    TooManyRequests,
    AccountLocked,
    Suspended,
    RequiresVerification,
    Unknown,
};

struct MfaChallenge
{
    QString ticket;
    QString loginInstanceId;
    bool sms = false;
    bool totp = false;
    bool backup = false;
};

class LoginClient : public QObject
{
    Q_OBJECT
public:
    explicit LoginClient(CaptchaResolver *captchaResolver = nullptr, QObject *parent = nullptr);
    ~LoginClient();

    void startLogin(const QString &login, const QString &password, const Core::ProxyConfig &proxy);

    void submitMfa(const QString &code, const QString &method);

    void sendSmsCode();

    void cancel();

signals:
    void mfaRequired(const MfaChallenge &challenge);
    void authenticated(const QString &token, const QString &userId, const QString &username,
                       const QString &displayName, const QString &avatar);
    void failed(LoginError error);
    void smsSent(bool ok);

private:
    struct Response
    {
        long httpCode = 0;
        QByteArray body;
        bool ok = false;
    };

    Response post(const QString &path, const QJsonObject &body, std::optional<CaptchaSolution> solution);
    Response fetchProfile(const QString &token);

    void performLogin(QString login, QString password, Core::ProxyConfig proxy,
                      std::optional<CaptchaSolution> solution, int attempt);
    void handleLoginResult(QString login, QString password, Core::ProxyConfig proxy,
                           std::optional<CaptchaSolution> solution, int attempt, Response response,
                           QString token, QString userId, QString username, QString displayName,
                           QString avatar);

    void performMfa(QString endpoint, QJsonObject body, std::optional<CaptchaSolution> solution,
                    int attempt);
    void handleMfaResult(std::optional<CaptchaSolution> solution, int attempt, Response response,
                         QString token, QString userId, QString username, QString displayName,
                         QString avatar);

    void startSms();

    void fail(LoginError error, bool terminal = true);
    void finish(const QString &token, const QString &userId, const QString &username,
                const QString &displayName, const QString &avatar);

    CaptchaResolver *captchaResolver = nullptr;

    Core::ProxyConfig proxy;
    QString mfaTicket;
    QString mfaLoginInstanceId;
    QString mfaEndpoint;
    QJsonObject mfaBody;

    std::atomic<bool> done{ false };
    std::thread worker;
};

} // namespace Discord
} // namespace Acheron