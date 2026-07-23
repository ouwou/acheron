#include "RemoteAuthClient.hpp"

#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"

#include "Core/Logging.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#ifdef _WIN32
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdint>
#include <string>

namespace Acheron {
namespace Discord {

static constexpr const char *gatewayUrl = "wss://remote-auth-gateway.discord.gg/?v=2";
static constexpr const char *loginUrl = "https://discord.com/api/v9/users/@me/remote-auth/login";

RemoteAuthClient::RemoteAuthClient(CaptchaResolver *captchaResolver, QObject *parent)
    : QObject(parent), captchaResolver(captchaResolver)
{
}

RemoteAuthClient::~RemoteAuthClient()
{
    stop();
    freeKey();
}

void RemoteAuthClient::start()
{
    if (running.exchange(true))
        return;

    done = false;

    if (!generateKey()) {
        running = false;
        fail(RemoteAuthError::KeyGenerationFailed);
        return;
    }

    networkThread = std::thread(&RemoteAuthClient::networkLoop, this);
}

void RemoteAuthClient::stop()
{
    if (!running.exchange(false))
        return;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
    if (httpThread.joinable())
        httpThread.join();
}

bool RemoteAuthClient::generateKey()
{
    keyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!keyCtx)
        return false;
    if (EVP_PKEY_keygen_init(keyCtx) <= 0)
        return false;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx, 2048) <= 0)
        return false;
    if (EVP_PKEY_keygen(keyCtx, &pkey) <= 0)
        return false;

    decCtx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!decCtx)
        return false;
    if (EVP_PKEY_decrypt_init(decCtx) <= 0)
        return false;
    if (EVP_PKEY_CTX_set_rsa_padding(decCtx, RSA_PKCS1_OAEP_PADDING) <= 0)
        return false;
    if (EVP_PKEY_CTX_set_rsa_oaep_md(decCtx, EVP_sha256()) <= 0)
        return false;
    if (EVP_PKEY_CTX_set_rsa_mgf1_md(decCtx, EVP_sha256()) <= 0)
        return false;

    return true;
}

void RemoteAuthClient::freeKey()
{
    if (decCtx) {
        EVP_PKEY_CTX_free(decCtx);
        decCtx = nullptr;
    }
    if (pkey) {
        EVP_PKEY_free(pkey);
        pkey = nullptr;
    }
    if (keyCtx) {
        EVP_PKEY_CTX_free(keyCtx);
        keyCtx = nullptr;
    }
}

QByteArray RemoteAuthClient::publicKeyDer() const
{
    int len = i2d_PUBKEY(pkey, nullptr);
    if (len <= 0)
        return {};
    QByteArray out(len, Qt::Uninitialized);
    auto *p = reinterpret_cast<unsigned char *>(out.data());
    if (i2d_PUBKEY(pkey, &p) <= 0)
        return {};
    return out;
}

QByteArray RemoteAuthClient::decrypt(const QByteArray &in) const
{
    if (!decCtx || in.isEmpty())
        return {};

    const auto *inp = reinterpret_cast<const unsigned char *>(in.constData());
    size_t outlen = 0;
    if (EVP_PKEY_decrypt(decCtx, nullptr, &outlen, inp, in.size()) <= 0)
        return {};

    QByteArray out(static_cast<int>(outlen), Qt::Uninitialized);
    if (EVP_PKEY_decrypt(decCtx, reinterpret_cast<unsigned char *>(out.data()), &outlen, inp,
                         in.size()) <= 0)
        return {};

    out.resize(static_cast<int>(outlen));
    return out;
}

void RemoteAuthClient::networkLoop()
{
    curl = curl_easy_init();
    if (!curl) {
        QMetaObject::invokeMethod(
                this, [this] { fail(RemoteAuthError::ConnectionFailed); }, Qt::QueuedConnection);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, gatewayUrl);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    CurlUtils::applyCommonOptions(curl);

    curl_slist *headers = curl_slist_append(nullptr, "Origin: https://discord.com");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        qCWarning(LogDiscord) << "Remote auth connect failed:" << curl_easy_strerror(res);
        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
        QMetaObject::invokeMethod(this, [this] { fail(RemoteAuthError::ConnectionFailed); }, Qt::QueuedConnection);
        return;
    }

    char chunk[8192];
    size_t rlen = 0;
    const curl_ws_frame *meta = nullptr;
    QByteArray message;

    while (running) {
        {
            std::lock_guard lock(curlMutex);
            res = curl_ws_recv(curl, chunk, sizeof(chunk), &rlen, &meta);
        }

        if (res == CURLE_AGAIN || (res == CURLE_OK && !meta)) {
            CurlUtils::wsRecvWait(curl, curlMutex);
            continue;
        }

        if (res != CURLE_OK) {
            qCWarning(LogDiscord) << "Remote auth recv error:" << curl_easy_strerror(res);
            QMetaObject::invokeMethod(this, [this] { fail(RemoteAuthError::ConnectionFailed); }, Qt::QueuedConnection);
            break;
        }

        if (meta->flags & CURLWS_CLOSE) {
            int closeCode = 1000;
            if (rlen >= 2)
                closeCode = (static_cast<uint8_t>(chunk[0]) << 8) | static_cast<uint8_t>(chunk[1]);
            qCInfo(LogDiscord) << "Remote auth closed with code" << closeCode;

            if (closeCode >= 4000) {
                RemoteAuthError error;
                switch (closeCode) {
                case 4001:
                case 4002:
                    error = RemoteAuthError::HandshakeFailed;
                    break;
                case 4003:
                    error = RemoteAuthError::TimedOut;
                    break;
                default:
                    error = RemoteAuthError::ConnectionFailed;
                    break;
                }
                QMetaObject::invokeMethod(this, [this, error] { fail(error); }, Qt::QueuedConnection);
            }
            break;
        }

        if (meta->flags & (CURLWS_PING | CURLWS_PONG))
            continue;

        message.append(chunk, static_cast<int>(rlen));
        if (meta->bytesleft == 0) {
            QString text = QString::fromUtf8(message);
            message.clear();
            QMetaObject::invokeMethod(this, [this, text] { handleMessage(text); }, Qt::QueuedConnection);
        }
    }

    {
        std::lock_guard lock(curlMutex);
        if (curl) {
            uint8_t closePayload[2] = { 0x03, 0xE8 }; // 1000
            size_t sent = 0;
            curl_ws_send(curl, closePayload, sizeof(closePayload), &sent, 0, CURLWS_CLOSE);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
    }
}

void RemoteAuthClient::heartbeatLoop()
{
    while (running) {
        {
            std::unique_lock lock(heartbeatMutex);
            if (heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval.load()),
                                     [this] { return !running.load(); }))
                break;
        }

        QJsonObject hb;
        hb["op"] = "heartbeat";
        send(hb);
    }
}

void RemoteAuthClient::send(const QJsonObject &obj)
{
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    CurlUtils::wsSend(curl, curlMutex, data.constData(), data.size(), CURLWS_TEXT, "remote auth");
}

void RemoteAuthClient::handleMessage(const QString &text)
{
    if (done)
        return;

    QJsonObject obj = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString op = obj["op"].toString();

    if (op == "hello")
        handleHello(obj);
    else if (op == "nonce_proof")
        handleNonceProof(obj);
    else if (op == "pending_remote_init")
        handlePendingRemoteInit(obj);
    else if (op == "pending_ticket")
        handlePendingTicket(obj);
    else if (op == "pending_login")
        handlePendingLogin(obj);
    else if (op == "cancel")
        cancel();
    else if (op == "heartbeat_ack") {
        // ack
    } else {
        qCDebug(LogDiscord) << "Unknown remote auth opcode:" << op;
    }
}

void RemoteAuthClient::handleHello(const QJsonObject &obj)
{
    heartbeatInterval = obj["heartbeat_interval"].toInt();
    if (!heartbeatThread.joinable())
        heartbeatThread = std::thread(&RemoteAuthClient::heartbeatLoop, this);

    // just in case
    const int timeoutMs = obj["timeout_ms"].toInt();
    if (timeoutMs > 0)
        QTimer::singleShot(timeoutMs + 2000, this, [this] { fail(RemoteAuthError::TimedOut); });

    QJsonObject init;
    init["op"] = "init";
    init["encoded_public_key"] = QString::fromLatin1(publicKeyDer().toBase64());
    send(init);
}

void RemoteAuthClient::handleNonceProof(const QJsonObject &obj)
{
    QByteArray encryptedNonce = QByteArray::fromBase64(obj["encrypted_nonce"].toString().toLatin1());
    QByteArray nonce = decrypt(encryptedNonce);
    if (nonce.isEmpty()) {
        fail(RemoteAuthError::HandshakeFailed);
        return;
    }

    QByteArray proof = nonce.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QJsonObject reply;
    reply["op"] = "nonce_proof";
    reply["nonce"] = QString::fromLatin1(proof);
    send(reply);
}

void RemoteAuthClient::handlePendingRemoteInit(const QJsonObject &obj)
{
    const QString fingerprint = obj["fingerprint"].toString();

    // verify
    const QByteArray der = publicKeyDer();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_Digest(der.constData(), der.size(), digest, &digestLen, EVP_sha256(), nullptr) != 1) {
        qCWarning(LogDiscord) << "Remote auth: couldn't compute fingerprint digest";
        fail(RemoteAuthError::HandshakeFailed);
        return;
    }

    const QByteArray expected = QByteArray(reinterpret_cast<char *>(digest), digestLen)
                                        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (QString::fromLatin1(expected) != fingerprint) {
        qCWarning(LogDiscord) << "Remote auth fingerprint mismatch; aborting handshake";
        fail(RemoteAuthError::HandshakeFailed);
        return;
    }

    emit fingerprintReady(fingerprint);
}

void RemoteAuthClient::handlePendingTicket(const QJsonObject &obj)
{
    QByteArray enc = QByteArray::fromBase64(obj["encrypted_user_payload"].toString().toLatin1());
    QByteArray payload = decrypt(enc);
    if (payload.isEmpty())
        return;

    // id:discriminator:avatar:username
    const QStringList parts = QString::fromUtf8(payload).split(':');
    const QString userId = parts.value(0);
    const QString avatar = parts.value(2);
    const QString username = parts.value(3);

    emit pendingTicket(userId, username, avatar);
}

void RemoteAuthClient::handlePendingLogin(const QJsonObject &obj)
{
    if (httpThread.joinable())
        return;

    postLogin(obj["ticket"].toString(), std::nullopt, 0);
}

void RemoteAuthClient::postLogin(const QString &ticket, std::optional<CaptchaSolution> solution,
                                 int attempt)
{
    if (done)
        return;

    if (httpThread.joinable())
        httpThread.join();

    const std::string body =
            QJsonDocument(QJsonObject{ { "ticket", ticket } })
                    .toJson(QJsonDocument::Compact)
                    .toStdString();

    httpThread = std::thread([this, body, ticket, solution, attempt]() {
        CURL *curl = curl_easy_init();
        QByteArray response;
        long httpCode = 0;
        bool ok = false;

        if (curl) {
            CurlUtils::applyCommonOptions(curl);

            curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");

            // todo: this probably isnt a perfect mimicry of the real client
            ClientIdentity identity;
            CurlUtils::appendDiscordHeaders(&headers, identity, "https://discord.com/login");
            headers = curl_slist_append(headers, "Origin: https://discord.com");

            if (solution)
                appendCaptchaHeaders(headers, *solution);

            curl_easy_setopt(curl, CURLOPT_URL, loginUrl);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlUtils::writeToByteArray);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                ok = (httpCode >= 200 && httpCode < 300);
            } else {
                qCWarning(LogNetwork) << "Remote auth login request failed:"
                                      << curl_easy_strerror(res);
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        QMetaObject::invokeMethod(
                this,
                [this, ok, httpCode, response, ticket, attempt]() {
                    if (done)
                        return;

                    QJsonObject root = QJsonDocument::fromJson(response).object();

                    if (!ok) {
                        std::optional<CaptchaChallenge> challenge;
                        if (httpCode == 400)
                            challenge = CaptchaChallenge::fromResponseBody(response);

                        if (challenge && captchaResolver && attempt < kMaxCaptchaAttempts) {
                            QPointer<RemoteAuthClient> self(this);
                            captchaResolver->resolve(
                                    *challenge,
                                    [this, self, ticket, attempt](std::optional<CaptchaSolution> sol) {
                                        if (!self || done)
                                            return;
                                        if (!sol) {
                                            fail(RemoteAuthError::CaptchaRequired);
                                            return;
                                        }
                                        postLogin(ticket, sol, attempt + 1);
                                    });
                            return;
                        }

                        const bool captcha = challenge.has_value();
                        if (attempt > 0 && captchaResolver)
                            captchaResolver->notifyConcluded();
                        qCWarning(LogNetwork) << "Remote auth login failed, HTTP" << httpCode << (captcha ? "(captcha required)" : "");
                        fail(captcha ? RemoteAuthError::CaptchaRequired
                                     : RemoteAuthError::LoginFailed);
                        return;
                    }

                    if (attempt > 0 && captchaResolver)
                        captchaResolver->notifyConcluded();

                    QByteArray enc = QByteArray::fromBase64(root["encrypted_token"].toString().toLatin1());
                    QByteArray token = decrypt(enc);
                    if (token.isEmpty()) {
                        qCWarning(LogNetwork) << "Remote auth token decryption failed";
                        fail(RemoteAuthError::LoginFailed);
                        return;
                    }

                    succeed(QString::fromUtf8(token));
                },
                Qt::QueuedConnection);
    });
}

void RemoteAuthClient::fail(RemoteAuthError error)
{
    if (done.exchange(true))
        return;
    emit failed(error);
}

void RemoteAuthClient::succeed(const QString &token)
{
    if (done.exchange(true))
        return;
    emit authenticated(token);
}

void RemoteAuthClient::cancel()
{
    if (done.exchange(true))
        return;
    emit canceled();
}

} // namespace Discord
} // namespace Acheron
