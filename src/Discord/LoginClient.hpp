#pragma once

#include <QObject>
#include <QString>

#include "CaptchaResolver.hpp"
#include "ClientIdentity.hpp"
#include "HttpClient.hpp"
#include "Core/ProxyConfig.hpp"

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
    PasskeyOnly,
    Unknown,
};

enum class MfaMethod {
    Totp,
    Sms,
    Backup,
};

struct MfaChallenge
{
    bool totp = false;
    bool sms = false;
    bool backup = false;
    bool webauthn = false;
};

class LoginClient : public QObject
{
    Q_OBJECT
public:
    explicit LoginClient(const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr,
                         QObject *parent = nullptr);

    void login(const QString &login, const QString &password);
    void submitMfa(MfaMethod method, const QString &code);
    void requestSmsCode();
    [[nodiscard]] bool canRetryMfa() const { return !mfaTicket.isEmpty(); }

signals:
    void mfaRequired(const MfaChallenge &challenge);
    void authenticated(const QString &token);
    void failed(LoginError error);
    void smsSent(bool ok);

private:
    void fetchFingerprintThen(std::function<void()> next);
    void postLogin(const QString &login, const QString &password);
    void handleLoginResponse(const HttpResponse &response);
    void handleMfaResponse(const HttpResponse &response);
    [[nodiscard]] std::optional<LoginError> commonFailure(const HttpResponse &response, const QJsonObject &root) const;

    ClientIdentity identity;
    HttpClient http;
    bool haveFingerprint = false;
    QString mfaTicket;
    QString mfaLoginInstanceId;
};

} // namespace Discord
} // namespace Acheron
