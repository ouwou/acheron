#include "LoginClient.hpp"

#include "Core/Logging.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace Acheron {
namespace Discord {

static constexpr const char *apiBase = "https://discord.com/api/v9";
static constexpr const char *loginPage = "https://discord.com/login";

namespace ErrorCode {
constexpr int AccountScheduledForDeletion = 20011;
constexpr int AccountDisabled = 20013;
constexpr int MfaInvalidCode = 60008;
constexpr int PhoneVerificationRequired = 70007;
} // namespace ErrorCode

static QString mfaEndpoint(MfaMethod method)
{
    switch (method) {
    case MfaMethod::Totp:
        return "/auth/mfa/totp";
    case MfaMethod::Sms:
        return "/auth/mfa/sms";
    case MfaMethod::Backup:
        return "/auth/mfa/backup";
    }
    return {};
}

static bool invalidatesTicket(LoginError error)
{
    return error != LoginError::InvalidMfaCode && error != LoginError::Network;
}

static QString serverErrorMessage(const QJsonObject &root)
{
    const QJsonObject errors = root["errors"].toObject();
    for (const char *field : { "login", "email", "password", "code" }) {
        const QJsonArray fieldErrors = errors[QLatin1String(field)].toObject()["_errors"].toArray();
        if (!fieldErrors.isEmpty())
            return fieldErrors.first().toObject()["message"].toString();
    }
    return root["message"].toString();
}

LoginClient::LoginClient(const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), http(apiBase, QString(), identity, proxy, captchaResolver)
{
    http.setReferer(loginPage);
}

void LoginClient::login(const QString &login, const QString &password)
{
    mfaTicket.clear();
    mfaLoginInstanceId.clear();

    if (haveFingerprint)
        postLogin(login, password);
    else
        fetchFingerprintThen([this, login, password]() { postLogin(login, password); });
}

void LoginClient::fetchFingerprintThen(std::function<void()> next)
{
    QUrlQuery query;
    query.addQueryItem("with_guild_experiments", "true");
    http.get("/experiments", query, [this, next = std::move(next)](const HttpResponse &response) {
        const QString fingerprint = QJsonDocument::fromJson(response.body).object()["fingerprint"].toString();
        if (response.success && !fingerprint.isEmpty()) {
            http.setFingerprint(fingerprint);
            haveFingerprint = true;
        } else {
            qCWarning(LogDiscord) << "Login: fingerprint fetch failed, HTTP" << response.statusCode << response.error;
        }
        next();
    });
}

void LoginClient::postLogin(const QString &login, const QString &password)
{
    QJsonObject body;
    body["login"] = login;
    body["password"] = password;
    body["undelete"] = false;
    body["login_source"] = QJsonValue::Null;
    body["gift_code_sku_id"] = QJsonValue::Null;

    http.post("/auth/login", body, [this](const HttpResponse &response) { handleLoginResponse(response); });
}

void LoginClient::handleLoginResponse(const HttpResponse &response)
{
    const QJsonObject root = QJsonDocument::fromJson(response.body).object();

    if (response.success) {
        const QString token = root["token"].toString();
        if (!token.isEmpty()) {
            emit authenticated(token);
            return;
        }

        if (!root["mfa"].toBool()) {
            qCWarning(LogDiscord) << "Login: success response without token";
            emit failed(LoginError::Unknown);
            return;
        }

        MfaChallenge challenge;
        challenge.totp = root["totp"].toBool();
        challenge.sms = root["sms"].toBool();
        challenge.backup = root["backup"].toBool();
        challenge.webauthn = root["webauthn"].isString();

        if (!challenge.totp && !challenge.sms && !challenge.backup) {
            qCWarning(LogDiscord) << "Login: no supported MFA method offered";
            emit failed(challenge.webauthn ? LoginError::PasskeyOnly : LoginError::Unknown);
            return;
        }

        mfaTicket = root["ticket"].toString();
        mfaLoginInstanceId = root["login_instance_id"].toString();
        emit mfaRequired(challenge);
        return;
    }

    if (const std::optional<LoginError> error = commonFailure(response, root)) {
        emit failed(*error);
        return;
    }

    const QString message = serverErrorMessage(root);
    qCWarning(LogDiscord) << "Login failed, HTTP" << response.statusCode << message;

    switch (root["code"].toInt()) {
    case ErrorCode::AccountScheduledForDeletion:
    case ErrorCode::AccountDisabled:
        emit failed(LoginError::AccountLocked);
        return;
    case ErrorCode::PhoneVerificationRequired:
        emit failed(LoginError::RequiresVerification);
        return;
    default:
        break;
    }

    emit failed(response.statusCode == 400 ? LoginError::InvalidCredentials : LoginError::Unknown);
}

void LoginClient::submitMfa(MfaMethod method, const QString &code)
{
    if (mfaTicket.isEmpty() || code.isEmpty())
        return;

    QJsonObject body;
    body["code"] = code;
    body["ticket"] = mfaTicket;
    body["login_source"] = QJsonValue::Null;
    body["gift_code_sku_id"] = QJsonValue::Null;
    body["login_instance_id"] = mfaLoginInstanceId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(mfaLoginInstanceId);

    http.post(mfaEndpoint(method), body, [this](const HttpResponse &response) { handleMfaResponse(response); });
}

void LoginClient::handleMfaResponse(const HttpResponse &response)
{
    const QJsonObject root = QJsonDocument::fromJson(response.body).object();

    if (response.success) {
        const QString token = root["token"].toString();
        if (token.isEmpty()) {
            qCWarning(LogDiscord) << "Login: MFA response without token";
            emit failed(LoginError::Unknown);
            return;
        }
        emit authenticated(token);
        return;
    }

    std::optional<LoginError> error = commonFailure(response, root);
    if (!error) {
        const QString message = serverErrorMessage(root);
        qCWarning(LogDiscord) << "MFA failed, HTTP" << response.statusCode << message;
        error = root["code"].toInt() == ErrorCode::MfaInvalidCode ? LoginError::InvalidMfaCode : LoginError::Unknown;
    }

    if (invalidatesTicket(*error))
        mfaTicket.clear();
    emit failed(*error);
}

void LoginClient::requestSmsCode()
{
    if (mfaTicket.isEmpty())
        return;

    http.post("/auth/mfa/sms/send", QJsonObject{ { "ticket", mfaTicket } }, [this](const HttpResponse &response) {
        if (!response.success)
            qCWarning(LogDiscord) << "Login: SMS send failed, HTTP" << response.statusCode << response.error;
        emit smsSent(response.success);
    });
}

std::optional<LoginError> LoginClient::commonFailure(const HttpResponse &response, const QJsonObject &root) const
{
    if (response.statusCode == 0) {
        qCWarning(LogNetwork) << "Login request failed:" << response.error;
        return LoginError::Network;
    }

    // captcha failed despite going through the resolver
    if (CaptchaChallenge::fromResponseBody(response.body))
        return LoginError::CaptchaRequired;

    if (response.rateLimited()) {
        qCWarning(LogNetwork) << "Login rate limited, retry after" << root["retry_after"].toDouble();
        return LoginError::TooManyRequests;
    }

    if (root.contains("suspended_user_token"))
        return LoginError::Suspended;

    return std::nullopt;
}

} // namespace Discord
} // namespace Acheron
