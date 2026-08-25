// Tests for the leak-critical parts of per-account proxying. Four failure modes
// drive what is covered here:
//
//   1. Storage asymmetry. AccountRepository::parseProxy returns a default
//      ProxyConfig -- type None, meaning *direct* -- for any stored string that
//      will not re-parse. So any input toString() can produce that parse()
//      rejects silently downgrades that account to an unproxied connection on
//      the next launch. See roundTripsThroughStorage.
//   2. DNS leaks. Traffic can flow through the proxy while names are still
//      resolved locally: curl needs the socks5h scheme, Qt needs
//      HostNameLookupCapability. See curlUrlResolvesDnsAtProxy / qtProxyResolvesDnsAtProxy.
//   3. Fail-open parsing. A rejected input must be std::nullopt, never a
//      disabled config, or a caller that only checks enabled() connects direct.
//      See parseFailsClosed.
//   4. Misrouted voice. A wrong SOCKS5 UDP header sends RTP to the wrong host or
//      corrupts the payload. See the encapsulate/decapsulate cases.

#include "Core/ImageManager.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Voice/Socks5UdpAssociation.hpp"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QList>
#include <QTest>
#include <QtEndian>

#include <cstring>
#include <tuple>

using Acheron::Core::ImageManager;
using Acheron::Core::ProxyConfig;
using Acheron::Core::Snowflake;
using Acheron::Discord::Voice::Socks5UdpAssociation;

Q_DECLARE_METATYPE(ProxyConfig)

namespace {

ProxyConfig make(ProxyConfig::Type type, const QString &host, quint16 port,
                 const QString &username = {}, const QString &password = {})
{
    ProxyConfig config;
    config.type = type;
    config.host = host;
    config.port = port;
    config.username = username;
    config.password = password;
    return config;
}

// Decodes a SOCKS5 UDP request header so tests can assert on where a datagram
// was actually addressed, rather than trusting the encoder's own round-trip.
struct Header
{
    bool valid = false;
    quint8 fragment = 0;
    quint8 addressType = 0;
    QHostAddress destination;
    quint16 port = 0;
    QByteArray payload;
};

Header decodeHeader(const QByteArray &datagram)
{
    Header header;
    if (datagram.size() < 4)
        return header;

    header.fragment = quint8(datagram.at(2));
    header.addressType = quint8(datagram.at(3));

    int cursor = 4;
    if (header.addressType == 1) {
        if (datagram.size() < cursor + 4)
            return header;
        quint32 raw = 0;
        memcpy(&raw, datagram.constData() + cursor, 4);
        header.destination = QHostAddress(qFromBigEndian(raw));
        cursor += 4;
    } else if (header.addressType == 4) {
        if (datagram.size() < cursor + 16)
            return header;
        Q_IPV6ADDR raw;
        memcpy(raw.c, datagram.constData() + cursor, 16);
        header.destination = QHostAddress(raw);
        cursor += 16;
    } else {
        return header;
    }

    if (datagram.size() < cursor + 2)
        return header;

    quint16 portRaw = 0;
    memcpy(&portRaw, datagram.constData() + cursor, 2);
    header.port = qFromBigEndian(portRaw);
    header.payload = datagram.mid(cursor + 2);
    header.valid = true;
    return header;
}

QByteArray buildDatagram(quint8 fragment, quint8 addressType, const QByteArray &address,
                         quint16 port, const QByteArray &payload)
{
    QByteArray datagram;
    datagram.append(char(0));
    datagram.append(char(0));
    datagram.append(char(fragment));
    datagram.append(char(addressType));
    datagram.append(address);
    const quint16 portRaw = qToBigEndian(port);
    datagram.append(reinterpret_cast<const char *>(&portRaw), 2);
    datagram.append(payload);
    return datagram;
}

} // namespace

class TestProxy : public QObject
{
    Q_OBJECT
private slots:
    // --- parsing ---------------------------------------------------------
    void parseAcceptedForms_data();
    void parseAcceptedForms();
    void parseFailsClosed_data();
    void parseFailsClosed();
    void parseEmptyMeansDirect_data();
    void parseEmptyMeansDirect();

    // --- invariants used by callers --------------------------------------
    void enabledRequiresEveryField();
    void canRelayUdp_data();
    void canRelayUdp();
    void equalityIsFieldSensitive();

    // --- formatting ------------------------------------------------------
    void curlUrlResolvesDnsAtProxy_data();
    void curlUrlResolvesDnsAtProxy();
    void curlUrlOmitsCredentials();
    void displayStringHidesCredentials_data();
    void displayStringHidesCredentials();
    void formattersBracketIpv6();
    void formattersEmptyWhenDirect();

    // --- Qt proxy --------------------------------------------------------
    void qtProxyTypes_data();
    void qtProxyTypes();
    void qtProxyResolvesDnsAtProxy_data();
    void qtProxyResolvesDnsAtProxy();
    void qtProxyCarriesCredentials();

    // --- storage round trip ----------------------------------------------
    void roundTripsThroughStorage_data();
    void roundTripsThroughStorage();

    // --- per-account image routing ---------------------------------------
    void proxiedAccountGetsItsOwnManager();
    void accountsRouteIndependently();
    void changingProxySwapsRoute();
    void reapplyingSameProxyKeepsManager();
    void clearingProxyFallsBackToDirect();
    void incompleteProxyIsNotAppliedSilently();

    // --- SOCKS5 UDP framing ----------------------------------------------
    void encapsulateIpv4Layout();
    void encapsulateIpv6Layout();
    void encapsulateAddressesExactDestination_data();
    void encapsulateAddressesExactDestination();
    void encapsulatePreservesPayload_data();
    void encapsulatePreservesPayload();
    void decapsulateRoundTrip_data();
    void decapsulateRoundTrip();
    void decapsulateAcceptsDomainAddresses();
    void decapsulateRejectsMalformed_data();
    void decapsulateRejectsMalformed();
    void decapsulateRejectsFragments_data();
    void decapsulateRejectsFragments();
    void decapsulateSurvivesTruncationFuzz();
    void encapsulateRefusesUnknownDestination();
};

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

void TestProxy::parseAcceptedForms_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<ProxyConfig>("expected");

    const auto Http = ProxyConfig::Type::Http;
    const auto Https = ProxyConfig::Type::Https;
    const auto Socks5 = ProxyConfig::Type::Socks5;

    QTest::newRow("http") << "http://proxy.test:8080" << make(Http, "proxy.test", 8080);
    QTest::newRow("https") << "https://proxy.test:8443" << make(Https, "proxy.test", 8443);
    QTest::newRow("socks5") << "socks5://proxy.test:1080" << make(Socks5, "proxy.test", 1080);
    QTest::newRow("socks5h") << "socks5h://proxy.test:1080" << make(Socks5, "proxy.test", 1080);
    QTest::newRow("socks") << "socks://proxy.test:1080" << make(Socks5, "proxy.test", 1080);

    QTest::newRow("uppercase scheme") << "SOCKS5://proxy.test:1080"
                                      << make(Socks5, "proxy.test", 1080);
    QTest::newRow("surrounding space") << "  socks5://proxy.test:1080  "
                                       << make(Socks5, "proxy.test", 1080);

    QTest::newRow("ipv4 host") << "socks5://127.0.0.1:1080" << make(Socks5, "127.0.0.1", 1080);
    QTest::newRow("ipv6 host") << "socks5://[::1]:1080" << make(Socks5, "::1", 1080);

    QTest::newRow("lowest port") << "socks5://proxy.test:1" << make(Socks5, "proxy.test", 1);
    QTest::newRow("highest port") << "socks5://proxy.test:65535"
                                  << make(Socks5, "proxy.test", 65535);

    QTest::newRow("trailing slash") << "socks5://proxy.test:1080/"
                                    << make(Socks5, "proxy.test", 1080);
    QTest::newRow("ignored path") << "http://proxy.test:8080/proxy.pac"
                                  << make(Http, "proxy.test", 8080);
    QTest::newRow("uppercase host") << "socks5://PROXY.TEST:1080"
                                    << make(Socks5, "proxy.test", 1080);

    QTest::newRow("credentials") << "socks5://user:pass@proxy.test:1080"
                                 << make(Socks5, "proxy.test", 1080, "user", "pass");
    QTest::newRow("encoded credentials")
            << "socks5://us%3Aer:p%40ss%2F1@proxy.test:1080"
            << make(Socks5, "proxy.test", 1080, "us:er", "p@ss/1");
    QTest::newRow("username only") << "http://user@proxy.test:8080"
                                   << make(Http, "proxy.test", 8080, "user");
}

void TestProxy::parseAcceptedForms()
{
    QFETCH(QString, input);
    QFETCH(ProxyConfig, expected);

    const auto parsed = ProxyConfig::parse(input);
    QVERIFY2(parsed.has_value(), qPrintable("rejected: " + input));
    QCOMPARE(parsed->type, expected.type);
    QCOMPARE(parsed->host, expected.host);
    QCOMPARE(parsed->port, expected.port);
    QCOMPARE(parsed->username, expected.username);
    QCOMPARE(parsed->password, expected.password);
    QVERIFY(parsed->enabled());
}

void TestProxy::parseFailsClosed_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("socks4") << "socks4://proxy.test:1080";
    QTest::newRow("socks4a") << "socks4a://proxy.test:1080";
    QTest::newRow("ftp") << "ftp://proxy.test:21";
    QTest::newRow("file") << "file:///etc/passwd";
    QTest::newRow("javascript") << "javascript:alert(1)";
    QTest::newRow("no scheme") << "proxy.test:1080";
    QTest::newRow("empty scheme") << "://proxy.test:1080";
    QTest::newRow("no port") << "socks5://proxy.test";
    QTest::newRow("port zero") << "socks5://proxy.test:0";
    QTest::newRow("port too high") << "socks5://proxy.test:65536";
    QTest::newRow("negative port") << "socks5://proxy.test:-1";
    QTest::newRow("non numeric port") << "socks5://proxy.test:http";
    QTest::newRow("no host") << "socks5://:1080";
    QTest::newRow("scheme only") << "socks5://";
    QTest::newRow("prose") << "my proxy is at home";
    QTest::newRow("host only") << "proxy.test";
}

void TestProxy::parseFailsClosed()
{
    QFETCH(QString, input);

    const auto parsed = ProxyConfig::parse(input);

    // Not merely "not enabled": a value would be a *valid* no-proxy config, and
    // callers treat that as a deliberate direct connection.
    QVERIFY2(!parsed.has_value(), qPrintable("accepted: " + input));
}

void TestProxy::parseEmptyMeansDirect_data()
{
    QTest::addColumn<QString>("input");
    QTest::newRow("empty") << "";
    QTest::newRow("spaces") << "   ";
    QTest::newRow("tab and newline") << "\t\n";
}

void TestProxy::parseEmptyMeansDirect()
{
    QFETCH(QString, input);

    const auto parsed = ProxyConfig::parse(input);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->type, ProxyConfig::Type::None);
    QVERIFY(!parsed->enabled());
    QVERIFY(parsed->toCurlUrl().isEmpty());
}

// ---------------------------------------------------------------------------
// invariants
// ---------------------------------------------------------------------------

void TestProxy::enabledRequiresEveryField()
{
    QVERIFY(make(ProxyConfig::Type::Socks5, "h", 1080).enabled());

    QVERIFY(!ProxyConfig().enabled());
    QVERIFY(!make(ProxyConfig::Type::None, "h", 1080).enabled());
    QVERIFY(!make(ProxyConfig::Type::Socks5, QString(), 1080).enabled());
    QVERIFY(!make(ProxyConfig::Type::Socks5, "h", 0).enabled());
}

void TestProxy::canRelayUdp_data()
{
    QTest::addColumn<ProxyConfig>("config");
    QTest::addColumn<bool>("expected");

    QTest::newRow("no proxy") << ProxyConfig() << true;
    QTest::newRow("socks5") << make(ProxyConfig::Type::Socks5, "h", 1080) << true;
    QTest::newRow("http") << make(ProxyConfig::Type::Http, "h", 8080) << false;
    QTest::newRow("https") << make(ProxyConfig::Type::Https, "h", 8443) << false;

    // A half-filled http config is not enabled, so nothing is proxied and UDP
    // goes direct like any unproxied account would.
    QTest::newRow("incomplete http") << make(ProxyConfig::Type::Http, "h", 0) << true;
}

void TestProxy::canRelayUdp()
{
    QFETCH(ProxyConfig, config);
    QFETCH(bool, expected);
    QCOMPARE(config.canRelayUdp(), expected);
}

void TestProxy::equalityIsFieldSensitive()
{
    // roundTripsThroughStorage leans on operator==, so it must notice every field.
    const ProxyConfig base = make(ProxyConfig::Type::Socks5, "h", 1080, "user", "pass");

    QVERIFY(base == make(ProxyConfig::Type::Socks5, "h", 1080, "user", "pass"));
    QVERIFY(!(base == make(ProxyConfig::Type::Http, "h", 1080, "user", "pass")));
    QVERIFY(!(base == make(ProxyConfig::Type::Socks5, "other", 1080, "user", "pass")));
    QVERIFY(!(base == make(ProxyConfig::Type::Socks5, "h", 1081, "user", "pass")));
    QVERIFY(!(base == make(ProxyConfig::Type::Socks5, "h", 1080, "other", "pass")));
    QVERIFY(!(base == make(ProxyConfig::Type::Socks5, "h", 1080, "user", "other")));
}

// ---------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------

void TestProxy::curlUrlResolvesDnsAtProxy_data()
{
    QTest::addColumn<ProxyConfig>("config");
    QTest::addColumn<QString>("expected");

    QTest::newRow("socks5") << make(ProxyConfig::Type::Socks5, "proxy.test", 1080)
                            << "socks5h://proxy.test:1080";
    QTest::newRow("http") << make(ProxyConfig::Type::Http, "proxy.test", 8080)
                          << "http://proxy.test:8080";
    QTest::newRow("https") << make(ProxyConfig::Type::Https, "proxy.test", 8443)
                           << "https://proxy.test:8443";
}

void TestProxy::curlUrlResolvesDnsAtProxy()
{
    QFETCH(ProxyConfig, config);
    QFETCH(QString, expected);

    const QString url = config.toCurlUrl();
    QCOMPARE(url, expected);

    // socks5:// would make libcurl resolve the hostname locally, leaking DNS
    // even though the traffic itself is proxied.
    if (config.type == ProxyConfig::Type::Socks5)
        QVERIFY(url.startsWith(QStringLiteral("socks5h://")));
}

void TestProxy::curlUrlOmitsCredentials()
{
    // Credentials travel via CURLOPT_PROXYUSERPWD; embedding them in the URL
    // would put the password into every verbose curl log.
    const ProxyConfig config =
            make(ProxyConfig::Type::Socks5, "proxy.test", 1080, "user", "secret");
    const QString url = config.toCurlUrl();

    QVERIFY(!url.contains(QStringLiteral("secret")));
    QVERIFY(!url.contains(QStringLiteral("user")));
    QVERIFY(!url.contains(QLatin1Char('@')));
    QCOMPARE(url, QStringLiteral("socks5h://proxy.test:1080"));
}

void TestProxy::displayStringHidesCredentials_data()
{
    QTest::addColumn<ProxyConfig>("config");
    QTest::addColumn<QString>("expected");

    QTest::newRow("socks5") << make(ProxyConfig::Type::Socks5, "proxy.test", 1080, "user", "secret")
                            << "socks5://proxy.test:1080";
    QTest::newRow("http") << make(ProxyConfig::Type::Http, "proxy.test", 8080, "user", "secret")
                          << "http://proxy.test:8080";
    QTest::newRow("no credentials") << make(ProxyConfig::Type::Https, "proxy.test", 8443)
                                    << "https://proxy.test:8443";
}

void TestProxy::displayStringHidesCredentials()
{
    QFETCH(ProxyConfig, config);
    QFETCH(QString, expected);

    const QString shown = config.toDisplayString();
    QCOMPARE(shown, expected);
    QVERIFY(!shown.contains(QStringLiteral("secret")));
    QVERIFY(!shown.contains(QLatin1Char('@')));

    // socks5, not socks5h: this string is shown to a person, not handed to curl.
    if (config.type == ProxyConfig::Type::Socks5)
        QVERIFY(!shown.contains(QStringLiteral("socks5h")));
}

void TestProxy::formattersBracketIpv6()
{
    const ProxyConfig config = make(ProxyConfig::Type::Socks5, "::1", 1080, "user", "pass");

    QCOMPARE(config.toCurlUrl(), QStringLiteral("socks5h://[::1]:1080"));
    QCOMPARE(config.toDisplayString(), QStringLiteral("socks5://[::1]:1080"));
    QCOMPARE(config.toString(), QStringLiteral("socks5://user:pass@[::1]:1080"));

    // An unbracketed literal cannot be told apart from a trailing port, so it
    // would fail to re-parse and silently become a direct connection.
    const auto reparsed = ProxyConfig::parse(config.toString());
    QVERIFY(reparsed.has_value());
    QVERIFY(*reparsed == config);
}

void TestProxy::formattersEmptyWhenDirect()
{
    const ProxyConfig direct;
    QVERIFY(direct.toCurlUrl().isEmpty());
    QVERIFY(direct.toString().isEmpty());
    QVERIFY(direct.toDisplayString().isEmpty());

    // Half-filled configs are not enabled, and must not produce a usable URL.
    QVERIFY(make(ProxyConfig::Type::Socks5, QString(), 1080).toCurlUrl().isEmpty());
    QVERIFY(make(ProxyConfig::Type::Socks5, "h", 0).toCurlUrl().isEmpty());
}

// ---------------------------------------------------------------------------
// Qt proxy
// ---------------------------------------------------------------------------

void TestProxy::qtProxyTypes_data()
{
    QTest::addColumn<ProxyConfig>("config");
    QTest::addColumn<int>("expected");

    QTest::newRow("socks5") << make(ProxyConfig::Type::Socks5, "h", 1080)
                            << int(QNetworkProxy::Socks5Proxy);
    QTest::newRow("http") << make(ProxyConfig::Type::Http, "h", 8080)
                          << int(QNetworkProxy::HttpProxy);

    // Qt has no TLS-to-proxy transport, so https deliberately degrades to a
    // plain CONNECT. Traffic still goes through the proxy; only the hop to the
    // proxy is unencrypted.
    QTest::newRow("https degrades") << make(ProxyConfig::Type::Https, "h", 8443)
                                    << int(QNetworkProxy::HttpProxy);

    QTest::newRow("none") << ProxyConfig() << int(QNetworkProxy::NoProxy);
    QTest::newRow("incomplete") << make(ProxyConfig::Type::Socks5, "h", 0)
                                << int(QNetworkProxy::NoProxy);
}

void TestProxy::qtProxyTypes()
{
    QFETCH(ProxyConfig, config);
    QFETCH(int, expected);

    const QNetworkProxy proxy = config.toQtProxy();
    QCOMPARE(int(proxy.type()), expected);

    if (config.enabled()) {
        QCOMPARE(proxy.hostName(), config.host);
        QCOMPARE(proxy.port(), config.port);
    }
}

void TestProxy::qtProxyResolvesDnsAtProxy_data()
{
    QTest::addColumn<ProxyConfig>("config");

    QTest::newRow("socks5") << make(ProxyConfig::Type::Socks5, "h", 1080);
    QTest::newRow("http") << make(ProxyConfig::Type::Http, "h", 8080);
    QTest::newRow("https") << make(ProxyConfig::Type::Https, "h", 8443);
}

void TestProxy::qtProxyResolvesDnsAtProxy()
{
    QFETCH(ProxyConfig, config);

    // Without this capability Qt resolves image hostnames locally, which leaks
    // DNS for every avatar and attachment even though the fetch is proxied.
    const QNetworkProxy proxy = config.toQtProxy();
    QVERIFY(proxy.capabilities().testFlag(QNetworkProxy::HostNameLookupCapability));
}

void TestProxy::qtProxyCarriesCredentials()
{
    const QNetworkProxy authenticated =
            make(ProxyConfig::Type::Socks5, "h", 1080, "user", "pass").toQtProxy();
    QCOMPARE(authenticated.user(), QStringLiteral("user"));
    QCOMPARE(authenticated.password(), QStringLiteral("pass"));

    const QNetworkProxy anonymous = make(ProxyConfig::Type::Socks5, "h", 1080).toQtProxy();
    QVERIFY(anonymous.user().isEmpty());
    QVERIFY(anonymous.password().isEmpty());
}

// ---------------------------------------------------------------------------
// storage round trip
// ---------------------------------------------------------------------------

void TestProxy::roundTripsThroughStorage_data()
{
    QTest::addColumn<ProxyConfig>("config");

    const QList<QPair<QByteArray, ProxyConfig::Type>> types = {
        { "http", ProxyConfig::Type::Http },
        { "https", ProxyConfig::Type::Https },
        { "socks5", ProxyConfig::Type::Socks5 },
    };
    const QList<QPair<QByteArray, QString>> hosts = {
        { "name", QStringLiteral("proxy.test") },
        { "subdomain", QStringLiteral("a.b.c.example.com") },
        { "ipv4", QStringLiteral("127.0.0.1") },
        { "ipv6", QStringLiteral("::1") },
        { "ipv6 full", QStringLiteral("2001:db8::dead:beef") },
    };
    const QList<std::tuple<QByteArray, QString, QString>> credentials = {
        { "anonymous", QString(), QString() },
        { "simple", QStringLiteral("user"), QStringLiteral("pass") },
        { "user only", QStringLiteral("user"), QString() },
        { "reserved chars", QStringLiteral("us:er"), QStringLiteral("p@ss/w?rd#1") },
        { "space", QStringLiteral("us er"), QStringLiteral("pa ss") },
        { "unicode", QStringLiteral("ユーザー"), QStringLiteral("пароль") },
        { "percent", QStringLiteral("us%er"), QStringLiteral("p%40ss") },
    };

    for (const auto &type : types) {
        for (const auto &host : hosts) {
            for (const auto &credential : credentials) {
                const QByteArray name =
                        type.first + " / " + host.first + " / " + std::get<0>(credential);
                QTest::newRow(name.constData())
                        << make(type.second, host.second, 1080, std::get<1>(credential),
                                std::get<2>(credential));
            }
        }
    }
}

void TestProxy::roundTripsThroughStorage()
{
    QFETCH(ProxyConfig, config);

    // This is exactly what the accounts table stores and reloads. A mismatch
    // here means the account comes back unproxied after a restart.
    const QString stored = config.toString();
    const auto reloaded = ProxyConfig::parse(stored);

    QVERIFY2(reloaded.has_value(), qPrintable("did not re-parse: " + stored));
    QVERIFY2(*reloaded == config, qPrintable("changed across storage: " + stored + " -> "
                                             + reloaded->toString()));
    QVERIFY(reloaded->enabled());

    // Idempotent: re-serialising must not drift either.
    QCOMPARE(reloaded->toString(), stored);
}

// ---------------------------------------------------------------------------
// per-account image routing
//
// ImageManager is the only Qt-side egress. Every account gets its own
// QNetworkAccessManager, and the one for a proxied account must carry that proxy
// or avatars, emoji and attachments fetch directly.
// ---------------------------------------------------------------------------

void TestProxy::proxiedAccountGetsItsOwnManager()
{
    ImageManager images;
    const Snowflake account(1);
    const ProxyConfig config =
            make(ProxyConfig::Type::Socks5, "proxy.test", 1080, "user", "pass");

    images.setAccountProxy(account, config);

    QNetworkAccessManager *manager = images.networkManagerFor(account);
    QVERIFY(manager != nullptr);
    QVERIFY(images.networkManagerFor(Snowflake()) == nullptr);

    const QNetworkProxy applied = manager->proxy();
    QCOMPARE(applied.type(), QNetworkProxy::Socks5Proxy);
    QCOMPARE(applied.hostName(), config.host);
    QCOMPARE(applied.port(), config.port);
    QCOMPARE(applied.user(), config.username);
    QCOMPARE(applied.password(), config.password);
    QVERIFY(applied.capabilities().testFlag(QNetworkProxy::HostNameLookupCapability));
}

void TestProxy::accountsRouteIndependently()
{
    ImageManager images;
    const Snowflake first(1);
    const Snowflake second(2);

    images.setAccountProxy(first, make(ProxyConfig::Type::Socks5, "first.test", 1080));
    images.setAccountProxy(second, make(ProxyConfig::Type::Http, "second.test", 8080));

    QNetworkAccessManager *firstManager = images.networkManagerFor(first);
    QNetworkAccessManager *secondManager = images.networkManagerFor(second);

    QVERIFY(firstManager != secondManager);
    QCOMPARE(firstManager->proxy().hostName(), QStringLiteral("first.test"));
    QCOMPARE(secondManager->proxy().hostName(), QStringLiteral("second.test"));

    // An unconfigured account gets no route at all, so it cannot inherit either proxy.
    QVERIFY(images.networkManagerFor(Snowflake(3)) == nullptr);
}

void TestProxy::changingProxySwapsRoute()
{
    ImageManager images;
    const Snowflake account(1);

    images.setAccountProxy(account, make(ProxyConfig::Type::Socks5, "old.test", 1080));
    images.setAccountProxy(account, make(ProxyConfig::Type::Socks5, "new.test", 1080));

    QCOMPARE(images.networkManagerFor(account)->proxy().hostName(), QStringLiteral("new.test"));
}

void TestProxy::reapplyingSameProxyKeepsManager()
{
    ImageManager images;
    const Snowflake account(1);
    const ProxyConfig config = make(ProxyConfig::Type::Socks5, "proxy.test", 1080);

    images.setAccountProxy(account, config);
    QNetworkAccessManager *manager = images.networkManagerFor(account);

    // Re-applying an unchanged proxy must not churn the manager, or every
    // in-flight image fetch would be cancelled.
    images.setAccountProxy(account, config);
    QCOMPARE(images.networkManagerFor(account), manager);
}

void TestProxy::clearingProxyFallsBackToDirect()
{
    ImageManager images;
    const Snowflake account(1);

    images.setAccountProxy(account, make(ProxyConfig::Type::Socks5, "proxy.test", 1080));
    QCOMPARE(images.networkManagerFor(account)->proxy().type(), QNetworkProxy::Socks5Proxy);

    images.setAccountProxy(account, ProxyConfig());
    QCOMPARE(images.networkManagerFor(account)->proxy().type(), QNetworkProxy::NoProxy);
}

void TestProxy::incompleteProxyIsNotAppliedSilently()
{
    ImageManager images;
    const Snowflake account(1);

    // Half-filled configs are not enabled, so they route directly. parse() never
    // produces one, which is why parseFailsClosed matters.
    images.setAccountProxy(account, make(ProxyConfig::Type::Socks5, "proxy.test", 0));
    QCOMPARE(images.networkManagerFor(account)->proxy().type(), QNetworkProxy::NoProxy);

    images.setAccountProxy(account, make(ProxyConfig::Type::Socks5, QString(), 1080));
    QCOMPARE(images.networkManagerFor(account)->proxy().type(), QNetworkProxy::NoProxy);
}

// ---------------------------------------------------------------------------
// SOCKS5 UDP framing
// ---------------------------------------------------------------------------

void TestProxy::encapsulateIpv4Layout()
{
    const QByteArray payload("audio");
    const QByteArray datagram =
            Socks5UdpAssociation::encapsulate(payload, QHostAddress("1.2.3.4"), 0x1234);

    QCOMPARE(datagram.size(), 10 + payload.size());
    QCOMPARE(datagram.mid(0, 3), QByteArray("\x00\x00\x00", 3)); // RSV + FRAG
    QCOMPARE(quint8(datagram.at(3)), quint8(1)); // ATYP = IPv4
    QCOMPARE(datagram.mid(4, 4), QByteArray("\x01\x02\x03\x04", 4));
    QCOMPARE(datagram.mid(8, 2), QByteArray("\x12\x34", 2)); // network byte order
    QCOMPARE(datagram.mid(10), payload);
}

void TestProxy::encapsulateIpv6Layout()
{
    const QByteArray payload("x");
    const QByteArray datagram =
            Socks5UdpAssociation::encapsulate(payload, QHostAddress("2001:db8::1"), 443);

    QCOMPARE(datagram.size(), 22 + payload.size());
    QCOMPARE(quint8(datagram.at(3)), quint8(4)); // ATYP = IPv6
    QCOMPARE(datagram.mid(4, 16),
             QByteArray("\x20\x01\x0d\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01", 16));
    QCOMPARE(datagram.mid(20, 2), QByteArray("\x01\xbb", 2));
    QCOMPARE(datagram.mid(22), payload);
}

void TestProxy::encapsulateAddressesExactDestination_data()
{
    QTest::addColumn<QString>("address");
    QTest::addColumn<quint16>("port");

    QTest::newRow("ipv4 low port") << "104.29.156.117" << quint16(1);
    QTest::newRow("ipv4 voice port") << "104.29.156.117" << quint16(19334);
    QTest::newRow("ipv4 high port") << "255.255.255.255" << quint16(65535);
    QTest::newRow("ipv4 zeros") << "0.0.0.0" << quint16(443);
    QTest::newRow("ipv6 loopback") << "::1" << quint16(19334);
    QTest::newRow("ipv6 full") << "2001:db8:85a3::8a2e:370:7334" << quint16(50000);
}

void TestProxy::encapsulateAddressesExactDestination()
{
    QFETCH(QString, address);
    QFETCH(quint16, port);

    const QHostAddress destination(address);
    const QByteArray datagram =
            Socks5UdpAssociation::encapsulate(QByteArray("payload"), destination, port);

    // Decoded independently of the encoder: a mistake here would relay voice to
    // the wrong host rather than fail visibly.
    const Header header = decodeHeader(datagram);
    QVERIFY(header.valid);
    QCOMPARE(header.fragment, quint8(0));
    QCOMPARE(header.destination, destination);
    QCOMPARE(header.port, port);
    QCOMPARE(header.payload, QByteArray("payload"));
}

void TestProxy::encapsulatePreservesPayload_data()
{
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("rtp header") << QByteArray("\x80\x78\x00\x01", 4);
    QTest::newRow("all zeros") << QByteArray(64, '\0');
    QTest::newRow("all ones") << QByteArray(64, '\xFF');
    QTest::newRow("header lookalike") << QByteArray("\x00\x00\x00\x01\x7f\x00\x00\x01\x04\x38", 10);
    QTest::newRow("mtu sized") << QByteArray(1400, 'a');
}

void TestProxy::encapsulatePreservesPayload()
{
    QFETCH(QByteArray, payload);

    const QByteArray datagram =
            Socks5UdpAssociation::encapsulate(payload, QHostAddress("1.2.3.4"), 19334);
    QCOMPARE(datagram.size(), 10 + payload.size());
    QCOMPARE(datagram.mid(10), payload);
}

void TestProxy::decapsulateRoundTrip_data()
{
    QTest::addColumn<QString>("address");
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("ipv4 rtp") << "8.8.8.8" << QByteArray("\x80\x78rtp-ish", 9);
    QTest::newRow("ipv6 rtp") << "2001:db8::1" << QByteArray("\x80\x78rtp-ish", 9);
    QTest::newRow("ipv4 zeros") << "8.8.8.8" << QByteArray(32, '\0');
    QTest::newRow("ipv6 mtu") << "2001:db8::1" << QByteArray(1400, 'z');
}

void TestProxy::decapsulateRoundTrip()
{
    QFETCH(QString, address);
    QFETCH(QByteArray, payload);

    const QByteArray wrapped =
            Socks5UdpAssociation::encapsulate(payload, QHostAddress(address), 19334);
    QCOMPARE(Socks5UdpAssociation::decapsulate(wrapped), payload);
}

void TestProxy::decapsulateAcceptsDomainAddresses()
{
    // Never sent by us, but a proxy may reply with a domain-form header and the
    // payload still has to be found at the right offset.
    QByteArray address;
    address.append(char(11));
    address.append("example.com");

    const QByteArray payload("\x80\x78opus", 6);
    const QByteArray datagram = buildDatagram(0, 3, address, 19334, payload);

    QCOMPARE(Socks5UdpAssociation::decapsulate(datagram), payload);
}

void TestProxy::decapsulateRejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("datagram");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("one byte") << QByteArray(1, '\0');
    QTest::newRow("below minimum") << QByteArray(9, '\0');
    QTest::newRow("unknown address type") << buildDatagram(0, 9, QByteArray(4, '\0'), 1, "x");
    QTest::newRow("address type zero") << buildDatagram(0, 0, QByteArray(4, '\0'), 1, "x");

    // ATYP says IPv6 but only an IPv4-sized address follows.
    QByteArray shortIpv6;
    shortIpv6.append("\x00\x00\x00\x04", 4);
    shortIpv6.append(QByteArray(8, '\0'));
    QTest::newRow("truncated ipv6") << shortIpv6;

    // Domain length claims far more than is present.
    QByteArray lyingDomain;
    lyingDomain.append("\x00\x00\x00\x03", 4);
    lyingDomain.append(char(200));
    lyingDomain.append("example.com");
    QTest::newRow("overlong domain length") << lyingDomain;

    // Domain header with no room left for the port.
    QByteArray noPort;
    noPort.append("\x00\x00\x00\x03", 4);
    noPort.append(char(8));
    noPort.append("hostname");
    QTest::newRow("domain without port") << noPort;
}

void TestProxy::decapsulateRejectsMalformed()
{
    QFETCH(QByteArray, datagram);
    QVERIFY(Socks5UdpAssociation::decapsulate(datagram).isEmpty());
}

void TestProxy::decapsulateRejectsFragments_data()
{
    QTest::addColumn<quint8>("fragment");

    QTest::newRow("first fragment") << quint8(1);
    QTest::newRow("middle fragment") << quint8(3);
    QTest::newRow("last fragment") << quint8(0x80);
}

void TestProxy::decapsulateRejectsFragments()
{
    QFETCH(quint8, fragment);

    // Reassembly is not implemented, so a fragment must be dropped rather than
    // handed to the decoder as if it were a whole packet.
    const QByteArray datagram =
            buildDatagram(fragment, 1, QByteArray("\x01\x02\x03\x04", 4), 19334, "payload");
    QVERIFY(Socks5UdpAssociation::decapsulate(datagram).isEmpty());
}

void TestProxy::decapsulateSurvivesTruncationFuzz()
{
    // Every prefix of a valid datagram must be handled without reading past the
    // end. The returned payload can never be longer than the bytes actually
    // present after the header, and an out-of-bounds read surfaces as a crash
    // here rather than mid-call.
    const QList<QPair<QString, int>> cases = {
        { QStringLiteral("1.2.3.4"), 10 },
        { QStringLiteral("2001:db8::1"), 22 },
    };

    for (const auto &entry : cases) {
        const QByteArray full = Socks5UdpAssociation::encapsulate(
                QByteArray("\x80\x78payload", 9), QHostAddress(entry.first), 19334);
        QCOMPARE(full.size(), entry.second + 9);

        for (int length = 0; length < full.size(); ++length) {
            const QByteArray result = Socks5UdpAssociation::decapsulate(full.left(length));
            const int available = qMax(0, length - entry.second);
            QVERIFY2(result.size() <= available,
                     qPrintable(QStringLiteral("prefix %1 of %2 returned %3 bytes")
                                        .arg(length).arg(entry.first).arg(result.size())));
        }
    }
}

void TestProxy::encapsulateRefusesUnknownDestination()
{
    // Discord hands us the voice address as a string; if it ever fails to parse,
    // the datagram must not end up aimed at something routable.
    const QByteArray datagram =
            Socks5UdpAssociation::encapsulate(QByteArray("x"), QHostAddress(), 19334);

    if (datagram.isEmpty())
        return;

    const Header header = decodeHeader(datagram);
    QVERIFY(header.valid);
    QVERIFY(header.destination.isNull()
            || header.destination == QHostAddress(QHostAddress::AnyIPv4)
            || header.destination == QHostAddress(QHostAddress::AnyIPv6));
}

QTEST_GUILESS_MAIN(TestProxy)
#include "tst_Proxy.moc"
