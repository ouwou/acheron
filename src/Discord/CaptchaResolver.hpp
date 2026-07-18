#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <curl/curl.h>

#include <functional>
#include <optional>

namespace Acheron {
namespace Discord {

inline constexpr int kMaxCaptchaAttempts = 3;

struct CaptchaChallenge
{
    QString service;
    QString sitekey;
    QString sessionId;
    QString rqdata;
    QString rqtoken;
    bool shouldServeInvisible = false;

    // parse a http 400 response body into a CaptchaChallenge if present
    static std::optional<CaptchaChallenge> fromResponseBody(const QByteArray &body);
};

struct CaptchaSolution
{
    QString token;
    QString sessionId;
    QString rqtoken;
};

inline void appendCaptchaHeaders(curl_slist *&headers, const CaptchaSolution &solution)
{
    headers = curl_slist_append(headers, ("X-Captcha-Key: " + solution.token).toUtf8().constData());
    if (!solution.sessionId.isEmpty())
        headers = curl_slist_append(headers, ("X-Captcha-Session-Id: " + solution.sessionId).toUtf8().constData());
    if (!solution.rqtoken.isEmpty())
        headers = curl_slist_append(headers, ("X-Captcha-Rqtoken: " + solution.rqtoken).toUtf8().constData());
}

class CaptchaResolver
{
public:
    using Callback = std::function<void(std::optional<CaptchaSolution>)>;

    virtual ~CaptchaResolver() = default;

    // asynchronously solve one challenge at a time. cb is invoked once
    virtual void resolve(const CaptchaChallenge &challenge, Callback cb) = 0;

    // called by the consumer after a success or give-up. a follow up resolve() signals a retry instead
    virtual void notifyConcluded() {}
};

inline std::optional<CaptchaChallenge> CaptchaChallenge::fromResponseBody(const QByteArray &body)
{
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    if (!root.value("captcha_key").isArray())
        return std::nullopt;

    CaptchaChallenge c;
    c.service = root.value("captcha_service").toString();
    c.sitekey = root.value("captcha_sitekey").toString();
    c.sessionId = root.value("captcha_session_id").toString();
    c.rqdata = root.value("captcha_rqdata").toString();
    c.rqtoken = root.value("captcha_rqtoken").toString();
    c.shouldServeInvisible = root.value("should_serve_invisible").toBool();
    return c;
}

} // namespace Discord
} // namespace Acheron
