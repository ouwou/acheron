#include "Discord/Entities.hpp"

#include <QJsonDocument>
#include <QTest>

using namespace Acheron::Discord;

class TestEmbeds : public QObject
{
    Q_OBJECT
private slots:
    void testGifvIsBareMedia();
    void testImageIsBareMediaWithoutTitleOrAuthor();
    void testOtherTypesAreNotBareMedia();
    void testMediaWithoutSizeDoesNotCount();
    void testOnlyALoneLinkIsHiddenBehindItsEmbed();
};

static Embed embed(const char *json)
{
    return Embed::fromJson(QJsonDocument::fromJson(json).object());
}

static const char *KlipyGifv = R"({"type":"gifv","url":"https://klipy.com/gifs/cat","provider":{"name":"Klipy"},
    "thumbnail":{"url":"https://static.klipy.com/cat.webp","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/static.klipy.com/cat.webp","width":498,"height":280},
    "video":{"url":"https://static.klipy.com/cat.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/static.klipy.com/cat.mp4","width":498,"height":280}})";

static const char *PlainImage = R"({"type":"image","url":"https://example.com/cat.png",
    "thumbnail":{"url":"https://example.com/cat.png","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/example.com/cat.png","width":640,"height":480}})";

void TestEmbeds::testGifvIsBareMedia()
{
    QVERIFY(embed(KlipyGifv).isBareMedia());
    QVERIFY(embed(R"({"type":"gifv","title":"Cat","author":{"name":"someone"},"thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"https://a/b.mp4","width":10,"height":10}})").isBareMedia());
    QVERIFY(embed(R"({"type":"gifv","video":{"url":"https://a/b.mp4","proxy_url":"https://p/b.mp4","width":10,"height":10}})").isBareMedia());

    QVERIFY(!embed(R"({"type":"gifv","video":{"url":"https://a/b.mp4","width":10,"height":10}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"http://a/b.mp4","width":10,"height":10}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
}

void TestEmbeds::testImageIsBareMediaWithoutTitleOrAuthor()
{
    QVERIFY(embed(PlainImage).isBareMedia());
    QVERIFY(embed(R"({"type":"image","image":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());

    QVERIFY(!embed(R"({"type":"image","title":"Cat","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"image","author":{"name":"someone"},"thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"image","url":"https://a/b.png"})").isBareMedia());
}

void TestEmbeds::testOtherTypesAreNotBareMedia()
{
    for (const char *type : { "rich", "article", "video", "link" }) {
        const QByteArray json = QByteArray(R"({"type":")") + type + R"(","image":{"url":"https://a/b.png","width":10,"height":10},"thumbnail":{"url":"https://a/b.png","width":10,"height":10}})";
        QVERIFY2(!embed(json.constData()).isBareMedia(), type);
    }
    QVERIFY(!embed(R"({"image":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
}

void TestEmbeds::testMediaWithoutSizeDoesNotCount()
{
    QVERIFY(!embed(R"({"type":"image","thumbnail":{"url":"https://a/b.png"}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"image","thumbnail":{"url":"https://a/b.png","width":0,"height":0}})").isBareMedia());
    QVERIFY(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"https://a/b.mp4","proxy_url":"https://p/b.mp4"}})").isBareMedia());
}

void TestEmbeds::testOnlyALoneLinkIsHiddenBehindItsEmbed()
{
    Message message;
    message.embeds = QList<Embed>{ embed(KlipyGifv) };
    QVERIFY(!message.hidesLinkBehindItsEmbed());

    message.contentIsSingleLink = true;
    QVERIFY(message.hidesLinkBehindItsEmbed());

    message.embeds = QList<Embed>{ embed(KlipyGifv), embed(PlainImage) };
    QVERIFY(!message.hidesLinkBehindItsEmbed());

    message.embeds = QList<Embed>{ embed(R"({"type":"article","title":"News","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})") };
    QVERIFY(!message.hidesLinkBehindItsEmbed());

    Message withoutEmbeds;
    withoutEmbeds.contentIsSingleLink = true;
    QVERIFY(!withoutEmbeds.hidesLinkBehindItsEmbed());
}

QTEST_GUILESS_MAIN(TestEmbeds)
#include "tst_Embeds.moc"
