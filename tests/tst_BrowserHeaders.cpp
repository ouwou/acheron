#include "Discord/ClientIdentity.hpp"
#include "Discord/CurlUtils.hpp"
#include "Discord/HttpClient.hpp"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QTest>

#include <curl/curl.h>

#include <atomic>
#include <thread>

using namespace Acheron;
using namespace Acheron::Discord;

namespace {

struct CapturedRequest
{
    QByteArray requestLine;
    QList<QByteArray> names;
    QHash<QByteArray, QByteArray> values; // lowercase name
};

class CapturingServer : public QObject
{
public:
    explicit CapturingServer(QByteArray (*respond)(const CapturedRequest &))
        : respond(respond)
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        });
        server.listen(QHostAddress::LocalHost);
    }

    quint16 port() const { return server.serverPort(); }

    QList<CapturedRequest> requests;

private:
    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers[socket];
        buffer += socket->readAll();
        const int end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;

        CapturedRequest request;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        request.requestLine = lines.first().trimmed();
        for (int i = 1; i < lines.size(); i++) {
            const QByteArray line = lines[i].trimmed();
            const int colon = line.indexOf(':');
            const QByteArray name = line.left(colon);
            // curl asks plain http for h2c, over https it's alpn
            if (name == "HTTP2-Settings" || (request.requestLine.contains("/api/") && (name == "Connection" || name == "Upgrade")))
                continue;
            request.names.append(name);
            request.values.insert(name.toLower(), line.mid(colon + 1).trimmed());
        }

        const qint64 bodyLength = request.values.value("content-length").toLongLong();
        if (buffer.size() < end + 4 + bodyLength)
            return;
        buffer.remove(0, end + 4 + bodyLength);

        requests.append(request);
        socket->write(respond(request));
    }

    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    QByteArray (*respond)(const CapturedRequest &);
};

QByteArray respondJson(const CapturedRequest &)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
}

QByteArray webSocketAccept(const CapturedRequest &request)
{
    const QByteArray key = request.values.value("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return QCryptographicHash::hash(key, QCryptographicHash::Sha1).toBase64();
}

QByteArray respondUpgrade(const CapturedRequest &request)
{
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + webSocketAccept(request) + "\r\n\r\n";
}

QByteArray respondUpgradeCompressed(const CapturedRequest &request)
{
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + webSocketAccept(request) + "\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
}

CURLcode handshakeOnThread(quint16 port, const QByteArray &host = "127.0.0.1")
{
    CURL *curl = curl_easy_init();
    const QByteArray url = "ws://" + host + ":" + QByteArray::number(port) + "/?v=9";
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    CurlUtils::applyCommonOptions(curl);

    std::atomic<CURLcode> result = CURLE_FAILED_INIT;
    std::thread worker([&] { result = CurlUtils::performWebSocketHandshake(curl, url); });
    // the server lives on this thread's event loop, the timeout is curl's
    [[maybe_unused]] const bool finished = QTest::qWaitFor([&] { return result != CURLE_FAILED_INIT; }, 10000);
    worker.join();
    curl_easy_cleanup(curl);
    return result;
}

} // namespace

class TestBrowserHeaders : public QObject
{
    Q_OBJECT

private slots:
    void apiGetLooksLikeFetch()
    {
        CapturingServer server(respondJson);
        ClientIdentity identity;
        HttpClient http(QString("http://127.0.0.1:%1/api/v9").arg(server.port()), "token", identity, {}, nullptr);

        bool done = false;
        http.get("/users/@me", QUrlQuery(), [&](const HttpResponse &) { done = true; });
        QVERIFY(QTest::qWaitFor([&] { return done; }, 10000));
        QCOMPARE(server.requests.size(), 1);

        const CapturedRequest &request = server.requests.first();
        const QList<QByteArray> expected = {
            "Host", "sec-ch-ua-platform", "Authorization", "X-Debug-Options", "sec-ch-ua", "sec-ch-ua-mobile",
            "X-Discord-Timezone", "X-Super-Properties", "X-Discord-Locale", "User-Agent", "Accept",
            "Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest", "Referer",
            "Accept-Encoding", "Accept-Language", "Priority"
        };
        QCOMPARE(request.names, expected);
        QCOMPARE(request.values.value("sec-ch-ua-platform"), QByteArray("\"Windows\""));
        QCOMPARE(request.values.value("accept"), QByteArray("*/*"));
        QCOMPARE(request.values.value("sec-fetch-mode"), QByteArray("cors"));
        QCOMPARE(request.values.value("sec-fetch-site"), QByteArray("same-origin"));
        QCOMPARE(request.values.value("priority"), QByteArray("u=1, i"));
        QCOMPARE(request.values.value("accept-language"), CurlUtils::getAcceptLanguage());
    }

    void apiPostCarriesOriginAndLength()
    {
        CapturingServer server(respondJson);
        ClientIdentity identity;
        HttpClient http(QString("http://127.0.0.1:%1/api/v9").arg(server.port()), "token", identity, {}, nullptr);

        bool done = false;
        http.post("/channels/1/messages", QJsonObject{ { "content", "hi" } }, [&](const HttpResponse &) { done = true; });
        QVERIFY(QTest::qWaitFor([&] { return done; }, 10000));
        QCOMPARE(server.requests.size(), 1);

        const CapturedRequest &request = server.requests.first();
        const QList<QByteArray> expected = {
            "Host", "Content-Length", "sec-ch-ua-platform", "Authorization", "X-Debug-Options", "sec-ch-ua", "sec-ch-ua-mobile",
            "X-Discord-Timezone", "X-Super-Properties", "X-Discord-Locale", "User-Agent", "Content-Type", "Accept", "Origin",
            "Sec-Fetch-Site", "Sec-Fetch-Mode", "Sec-Fetch-Dest", "Referer",
            "Accept-Encoding", "Accept-Language", "Priority"
        };
        QCOMPARE(request.names, expected);
        QCOMPARE(request.values.value("content-length"), QByteArray("16"));
        QCOMPARE(request.values.value("origin"), QByteArray("https://discord.com"));
    }

    // brave 158 fetch() captures with each of discord's header sets
    void rarerHeadersFollowChrome_data()
    {
        QTest::addColumn<QList<QByteArray>>("pageHeaders");
        QTest::addColumn<QList<QByteArray>>("expected");
        const QList<QByteArray> discord = { "Authorization: x", "X-Super-Properties: y", "X-Discord-Locale: en-US", "X-Discord-Timezone: tz", "X-Debug-Options: bugReporterEnabled" };
        QTest::newRow("message send") << (QList<QByteArray>{ "X-Context-Properties: c" } + discord)
                                      << QList<QByteArray>{ "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-context-properties", "x-super-properties", "x-discord-locale", "user-agent", "content-type", "accept" };
        QTest::newRow("audit log reason") << (QList<QByteArray>{ "X-Audit-Log-Reason: r" } + discord)
                                          << QList<QByteArray>{ "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-super-properties", "x-audit-log-reason", "x-discord-locale", "user-agent", "content-type", "accept" };
        QTest::newRow("attachment slot") << (QList<QByteArray>{ "X-Discord-Original-MD5: m" } + discord)
                                         << QList<QByteArray>{ "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-discord-original-md5", "x-super-properties", "x-discord-locale", "user-agent", "content-type", "accept" };
        const QList<QByteArray> captcha = { "X-Captcha-Key: k", "X-Captcha-Rqtoken: r", "X-Captcha-Session-Id: s" };
        QTest::newRow("captcha at login") << (captcha + QList<QByteArray>{ "X-Super-Properties: y", "X-Fingerprint: f", "X-Discord-Locale: en-US", "X-Discord-Timezone: tz", "X-Debug-Options: bugReporterEnabled" })
                                          << QList<QByteArray>{ "x-captcha-rqtoken", "sec-ch-ua-platform", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-captcha-key", "x-super-properties", "x-captcha-session-id", "x-discord-locale", "user-agent", "content-type", "x-fingerprint", "accept" };
        QTest::newRow("captcha logged in") << (QList<QByteArray>{ "X-Context-Properties: c" } + captcha + discord)
                                           << QList<QByteArray>{ "x-captcha-rqtoken", "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-captcha-key", "x-context-properties", "x-super-properties", "x-captcha-session-id", "x-discord-locale", "user-agent", "content-type", "accept" };
        QTest::newRow("empty audit log reason") << (QList<QByteArray>{ "X-Audit-Log-Reason;" } + discord)
                                                << QList<QByteArray>{ "sec-ch-ua-platform", "authorization", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-super-properties", "x-audit-log-reason;", "x-discord-locale", "user-agent", "content-type", "accept" };
        QTest::newRow("login") << QList<QByteArray>{ "X-Super-Properties: y", "X-Fingerprint: f", "X-Discord-Locale: en-US", "X-Discord-Timezone: tz", "X-Debug-Options: bugReporterEnabled" }
                               << QList<QByteArray>{ "sec-ch-ua-platform", "x-debug-options", "sec-ch-ua", "sec-ch-ua-mobile", "x-discord-timezone", "x-super-properties", "x-discord-locale", "user-agent", "content-type", "x-fingerprint", "accept" };
    }

    void rarerHeadersFollowChrome()
    {
        QFETCH(QList<QByteArray>, pageHeaders);
        QFETCH(QList<QByteArray>, expected);

        CurlUtils::BrowserRequest request;
        request.pageHeaders = pageHeaders;
        request.contentType = "application/json";
        curl_slist *headers = CurlUtils::browserHeaders(request);
        QList<QByteArray> names;
        for (curl_slist *it = headers; it && names.size() < expected.size(); it = it->next)
            names.append(QByteArray(it->data).split(':').first().toLower());
        curl_slist_free_all(headers);
        QCOMPARE(names, expected);
    }

    void acceptLanguageEndsInEnglish()
    {
        const QByteArray header = CurlUtils::getAcceptLanguage();
        QVERIFY(header.contains("en-US"));
        QVERIFY(!header.startsWith(','));
        QVERIFY(!header.contains(";q=0.0"));
        QCOMPARE(header.split(',').first(), CurlUtils::getSystemLocale().toUtf8());
    }

    void webSocketHandshakeLooksLikeChrome()
    {
        CapturingServer server(respondUpgrade);
        QCOMPARE(handshakeOnThread(server.port()), CURLE_OK);
        QCOMPARE(server.requests.size(), 1);

        const QList<QByteArray> expected = {
            "Host", "Pragma", "Cache-Control", "User-Agent", "Upgrade", "Origin",
            "Sec-WebSocket-Version", "Accept-Encoding", "Accept-Language",
            "Sec-WebSocket-Key", "Sec-WebSocket-Extensions"
        };
        // curl adds its own Connection, where depends on the curl
        QList<QByteArray> names = server.requests.first().names;
        QCOMPARE(names.removeAll("Connection"), 1);
        QCOMPARE(names, expected);
    }

    // last: it turns the offer off for 127.0.0.1
    void acceptedCompressionIsRefused()
    {
        CapturingServer server(respondUpgradeCompressed);
        QVERIFY(handshakeOnThread(server.port()) != CURLE_OK);

        CapturingServer plain(respondUpgrade);
        QCOMPARE(handshakeOnThread(plain.port()), CURLE_OK);
        QVERIFY(!plain.requests.first().values.contains("sec-websocket-extensions"));

        CapturingServer otherHost(respondUpgrade);
        QCOMPARE(handshakeOnThread(otherHost.port(), "localhost"), CURLE_OK);
        QVERIFY(otherHost.requests.first().values.contains("sec-websocket-extensions"));
    }
};

QTEST_GUILESS_MAIN(TestBrowserHeaders)
#include "tst_BrowserHeaders.moc"
