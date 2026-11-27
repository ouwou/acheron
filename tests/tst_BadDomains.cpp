#include <QCryptographicHash>
#include <QTest>

#include "Core/BadDomains.hpp"

using Acheron::Core::BadDomains;

class TestBadDomains : public QObject
{
    Q_OBJECT
private slots:
    void testMatching();
};

void TestBadDomains::testMatching()
{
    // same format as https://cdn.discordapp.com/bad-domains/hashes.json: hex SHA-256 of each domain
    auto hex = [](const char *domain) {
        return QString::fromLatin1(QCryptographicHash::hash(domain, QCryptographicHash::Sha256).toHex());
    };
    const QByteArray json = QStringLiteral(R"(["%1", "%2", "not a hash", "%1"])")
                                    .arg(hex("dlscord.gift"), hex("bad.example"))
                                    .toUtf8();
    const auto hashes = BadDomains::parseHashes(json);
    QCOMPARE(hashes.size(), size_t(2)); // the junk entry and the duplicate are dropped

    QVERIFY(BadDomains::matches(hashes, "dlscord.gift"));
    QVERIFY(BadDomains::matches(hashes, "DLSCORD.gift"));
    QVERIFY(BadDomains::matches(hashes, "dlscord.gift."));
    QVERIFY(BadDomains::matches(hashes, "www.dlscord.gift")); // any subdomain of a listed one
    QVERIFY(BadDomains::matches(hashes, "a.b.bad.example"));
    QVERIFY(!BadDomains::matches(hashes, "discord.gift"));
    QVERIFY(!BadDomains::matches(hashes, "example")); // never just the top-level domain
    QVERIFY(!BadDomains::matches(hashes, "notbad.example"));
    QVERIFY(!BadDomains::matches({}, "dlscord.gift"));
}

QTEST_MAIN(TestBadDomains)
#include "tst_BadDomains.moc"
