#include "LoginClient.hpp"

#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"

#include "Core/Logging.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include <string>

namespace Acheron {
namespace Discord {

static constexpr const char *apiBase = "https://discord.com/api/v9";

LoginClient::LoginClient(CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), captchaResolver(captchaResolver)
{
}

LoginClient::~LoginClient()
{
    cancel();
}

void LoginClient::startLogin(const QString &login, const QString &password,
                             const Core::ProxyConfig &proxy)
{
    if (done)
        return;

    this->proxy = proxy;
    mfaTicket.clear();
    mfaLoginInstanceId.clear();
    mfaEndpoint.clear();
    mfaBody = QJsonObject();

    performLogin(login, password, proxy, std::nullopt, 0);
}

void LoginClient::cancel()
{
    done = true;
    if (worker.joinable())
        worker.join();
}

LoginClient::Response LoginClient::post(const QString &path, const QJsonObject &body,
                                        std::optional<CaptchaSolution> solution)
{
    Response result;

    CURL *curl = curl_easy_init();
    if (!curl)
        return result;

    CurlUtils::applyCommonOptions(curl);
    CurlUtils::applyProxy(curl, proxy);

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    ClientIdentity identity;
    CurlUtils::appendDiscordHeaders(&headers, identity, "https://discord.com/login");
    headers = curl_slist_append(headers, "Origin: https://discord.com");

    if (solution)
        appendCaptchaHeaders(headers, *solution);

    const std::string url = (QString::fromLatin1(apiBase) + path).toStdString();
    const std::string data = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlUtils::writeToByteArray);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
        result.ok = (result.httpCode >= 200 && result.httpCode < 300);
    } else {
        qCWarning(LogNetwork) << "Login request failed:" << curl_easy_strerror(res);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

LoginClient::Response LoginClient::fetchProfile(const QString &token)
{
    Response result;

    CURL *curl = curl_easy_init();
    if (!curl)
        return result;

    CurlUtils::applyCommonOptions(curl);
    CurlUtils::applyProxy(curl, proxy);

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + token).toUtf8().constData());

    ClientIdentity identity;
    CurlUtils::appendDiscordHeaders(&headers, identity, "https://discord.com/channels/@me");

    const std::string url = (QString::fromLatin1(apiBase) + "/users/@me").toStdString();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlUtils::writeToByteArray);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpCode);
        result.ok = (result.httpCode >= 200 && result.httpCode < 300);
    } else {
        qCWarning(LogNetwork) << "Profile fetch failed:" << curl_easy_strerror(res);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

void LoginClient::performLogin(QString login, QString password, Core::ProxyConfig proxy,
                               std::optional<CaptchaSolution> solution, int attempt)
{
    if (done)
        return;

    if (worker.joinable())
        worker.join();

    mfaEndpoint.clear();
    mfaBody = QJsonObject();

    QJsonObject body;
    body["login"] = login;
    body["password"] = password;
    body["undelete"] = false;
    body["login_source"] = QJsonValue::Null;
    body["gift_code_sku_id"] = QJsonValue::Null;
    body["captcha_key"] = QJsonValue::Null;

    worker = std::thread([this, login, password, proxy, body = std::move(body),
                          solution = std::move(solution), attempt]() mutable {
        Response response = post("/auth/login", body, solution);

        QString token;
        QString userId;
        QString username;
        QString displayName;
        QString avatar;

        if (response.ok) {
            token = QJsonDocument::fromJson(response.body).object()["token"].toString();
            if (!token.isEmpty()) {
                const QJsonObject profile = QJsonDocument::fromJson(fetchProfile(token).body).object();
                userId = profile["id"].toString();
                username = profile["username"].toString();
                displayName = profile["global_name"].toString();
                avatar = profile["avatar"].toString();
                if (avatar == QLatin1String("0"))
                    avatar.clear();
            }
        }

        QMetaObject::invokeMethod(
                this,
                [this, login, password, proxy, solution = std::move(solution), attempt,
                 response = std::move(response), token, userId, username, displayName,
                 avatar]() mutable {
                    handleLoginResult(std::move(login), std::move(password), std::move(proxy),
                                      std::move(solution), attempt, std::move(response), token,
                                      userId, username, displayName, avatar);
                },
                Qt::QueuedConnection);
    });
}

void LoginClient::handleLoginResult(QString login, QString password, Core::ProxyConfig proxy,
                                    std::optional<CaptchaSolution> solution, int attempt,
                                    Response response, QString token, QString userId,
                                    QString username, QString displayName, QString avatar)
{
    if (done)
        return;

    if (response.ok) {
        if (!token.isEmpty()) {
            finish(std::move(token), std::move(userId), std::move(username),
                   std::move(displayName), std::move(avatar));
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(response.body).object();
        if (root.value("mfa").toBool()) {
            MfaChallenge challenge;
            challenge.ticket = root["ticket"].toString();
            challenge.loginInstanceId = root["login_instance_id"].toString();
            challenge.totp = root.contains("totp") ? root["totp"].toBool() : true;
            challenge.sms = root["sms"].toBool();
            challenge.backup = root["backup"].toBool();

            if (!challenge.totp && !challenge.sms && !challenge.backup) {
                qCWarning(LogDiscord) << "Login: unsupported MFA configuration";
                fail(LoginError::Unknown, false);
                return;
            }

            mfaTicket = challenge.ticket;
            mfaLoginInstanceId = challenge.loginInstanceId;
            emit mfaRequired(challenge);
            return;
        }

        qCWarning(LogDiscord) << "Login: unexpected success response without token";
        fail(LoginError::Unknown);
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(response.body).object();

    if (response.httpCode == 400 && root.contains("captcha_key")) {
        std::optional<CaptchaChallenge> challenge = CaptchaChallenge::fromResponseBody(response.body);
        if (challenge && captchaResolver && attempt < kMaxCaptchaAttempts) {
            QPointer<LoginClient> self(this);
            captchaResolver->resolve(*challenge, proxy,
                                     [this, self, login = std::move(login), password = std::move(password),
                                      proxy, attempt](std::optional<CaptchaSolution> sol) {
                                         if (!self || done)
                                             return;
                                         if (!sol) {
                                             fail(LoginError::CaptchaRequired, false);
                                             return;
                                         }
                                         performLogin(std::move(login), std::move(password),
                                                      proxy, std::move(sol), attempt + 1);
                                     });
            return;
        }

        if (attempt > 0 && captchaResolver)
            captchaResolver->notifyConcluded();
        qCWarning(LogNetwork) << "Login failed: captcha required";
        fail(LoginError::CaptchaRequired);
        return;
    }

    if (response.httpCode == 429) {
        qCWarning(LogNetwork) << "Login rate limited";
        fail(LoginError::TooManyRequests);
        return;
    }

    if (response.httpCode == 403 && root.contains("suspended_user_token")) {
        fail(LoginError::Suspended);
        return;
    }

    const int jsonCode = root["code"].toInt();
    if (response.httpCode == 403 && (jsonCode == 20011 || jsonCode == 20013)) {
        fail(LoginError::AccountLocked);
        return;
    }

    const QJsonObject errors = root["errors"].toObject();
    if (response.httpCode == 400 && !errors.isEmpty()) {
        const QJsonValue loginErrors = errors["login"];
        if (loginErrors.isObject()) {
            const QJsonArray arr = loginErrors.toObject()["_errors"].toArray();
            for (const QJsonValue &v : arr) {
                if (v.toObject()["code"].toString() == QLatin1String("LOGIN_VERIFICATION_REQUIRED")) {
                    fail(LoginError::RequiresVerification);
                    return;
                }
            }
        }

        QString message = root["message"].toString();
        if (message.isEmpty()) {
            const QJsonValue loginErrors = errors["login"];
            if (loginErrors.isObject()) {
                const QJsonArray arr = loginErrors.toObject()["_errors"].toArray();
                if (!arr.isEmpty())
                    message = arr.first().toObject()["message"].toString();
            }
        }
        if (message.isEmpty()) {
            const QJsonValue pwdErrors = errors["password"];
            if (pwdErrors.isObject()) {
                const QJsonArray arr = pwdErrors.toObject()["_errors"].toArray();
                if (!arr.isEmpty())
                    message = arr.first().toObject()["message"].toString();
            }
        }

        qCWarning(LogNetwork) << "Login failed:" << message;
        fail(LoginError::InvalidCredentials, false);
        return;
    }

    qCWarning(LogNetwork) << "Login failed, HTTP" << response.httpCode
                          << QJsonDocument::fromJson(response.body).object()["message"].toString();
    if (response.httpCode == 0)
        fail(LoginError::Network);
    else
        fail(LoginError::InvalidCredentials, false);
}

void LoginClient::submitMfa(const QString &code, const QString &method)
{
    if (done || code.isEmpty() || mfaTicket.isEmpty())
        return;

    if (method != QLatin1String("totp") && method != QLatin1String("sms") &&
        method != QLatin1String("backup")) {
        qCWarning(LogDiscord) << "Login: unknown MFA method" << method;
        return;
    }

    QJsonObject body;
    body["code"] = code;
    body["ticket"] = mfaTicket;
    if (!mfaLoginInstanceId.isEmpty())
        body["login_instance_id"] = mfaLoginInstanceId;

    performMfa("/auth/mfa/" + method, std::move(body), std::nullopt, 0);
}

void LoginClient::performMfa(QString endpoint, QJsonObject body,
                             std::optional<CaptchaSolution> solution, int attempt)
{
    if (done)
        return;

    if (worker.joinable())
        worker.join();

    mfaEndpoint = endpoint;
    mfaBody = body;

    worker = std::thread([this, endpoint = std::move(endpoint), body = std::move(body),
                          solution = std::move(solution), attempt]() mutable {
        Response response = post(endpoint, body, solution);

        QString token;
        QString userId;
        QString username;
        QString displayName;
        QString avatar;

        if (response.ok) {
            token = QJsonDocument::fromJson(response.body).object()["token"].toString();
            if (!token.isEmpty()) {
                const QJsonObject profile = QJsonDocument::fromJson(fetchProfile(token).body).object();
                userId = profile["id"].toString();
                username = profile["username"].toString();
                displayName = profile["global_name"].toString();
                avatar = profile["avatar"].toString();
                if (avatar == QLatin1String("0"))
                    avatar.clear();
            }
        }

        QMetaObject::invokeMethod(
                this,
                [this, solution = std::move(solution), attempt, response = std::move(response),
                 token, userId, username, displayName, avatar]() mutable {
                    handleMfaResult(std::move(solution), attempt, std::move(response), token,
                                    userId, username, displayName, avatar);
                },
                Qt::QueuedConnection);
    });
}

void LoginClient::handleMfaResult(std::optional<CaptchaSolution> solution, int attempt,
                                  Response response, QString token, QString userId,
                                  QString username, QString displayName, QString avatar)
{
    if (done)
        return;

    if (response.ok && !token.isEmpty()) {
        finish(std::move(token), std::move(userId), std::move(username), std::move(displayName),
               std::move(avatar));
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(response.body).object();

    if (response.httpCode == 400 && root.contains("captcha_key")) {
        std::optional<CaptchaChallenge> challenge = CaptchaChallenge::fromResponseBody(response.body);
        if (challenge && captchaResolver && attempt < kMaxCaptchaAttempts) {
            QPointer<LoginClient> self(this);
            captchaResolver->resolve(*challenge, proxy,
                                     [this, self, attempt](std::optional<CaptchaSolution> sol) {
                                         if (!self || done)
                                             return;
                                         if (!sol) {
                                             fail(LoginError::CaptchaRequired, false);
                                             return;
                                         }
                                         performMfa(mfaEndpoint, mfaBody, std::move(sol),
                                                    attempt + 1);
                                     });
            return;
        }
        if (attempt > 0 && captchaResolver)
            captchaResolver->notifyConcluded();
        fail(LoginError::CaptchaRequired);
        return;
    }

    const int jsonCode = root["code"].toInt();
    if (response.httpCode == 429) {
        fail(LoginError::TooManyRequests);
        return;
    }

    if (response.httpCode == 400 && (jsonCode == 60008 || root.contains("errors"))) {
        qCWarning(LogNetwork) << "Login: invalid MFA code";
        fail(LoginError::InvalidMfaCode, false);
        return;
    }

    qCWarning(LogNetwork) << "MFA verification failed, HTTP" << response.httpCode
                          << root["message"].toString();
    fail(response.httpCode == 0 ? LoginError::Network : LoginError::Unknown);
}

void LoginClient::sendSmsCode()
{
    startSms();
}

void LoginClient::startSms()
{
    if (done || mfaTicket.isEmpty())
        return;

    if (worker.joinable())
        worker.join();

    QJsonObject body;
    body["ticket"] = mfaTicket;

    worker = std::thread([this, body = std::move(body)]() {
        Response response = post("/auth/mfa/sms/send", body, std::nullopt);
        QMetaObject::invokeMethod(this, [this, ok = response.ok]() { emit smsSent(ok); },
                                  Qt::QueuedConnection);
    });
}

void LoginClient::fail(LoginError error, bool terminal)
{
    if (done)
        return;
    if (terminal)
        done = true;
    emit failed(error);
}

void LoginClient::finish(const QString &token, const QString &userId, const QString &username,
                         const QString &displayName, const QString &avatar)
{
    if (done)
        return;
    done = true;
    emit authenticated(token, userId, username, displayName, avatar);
}

} // namespace Discord
} // namespace Acheron