#include "Discord/CdnUrls.hpp"

#include <QTest>

using namespace Acheron::Discord;

class TestCdnUrls : public QObject
{
    Q_OBJECT
private slots:
    void testGifIsAnimatedWhateverItsFlags();
    void testWebpAndAvifNeedTheAnimatedFlag();
    void testExtensionMustBeFollowedByEndQueryOrFragment();

    void testAnimatedGifUrlAsksOnlyForItsSize();
    void testAnimatedGifUrlAtItsOwnSizeIsTheProxyUrl();
    void testAnimatedWebpUrlAsksForAnimation();
    void testAnimatedAvifUrlAsksForAnimatedWebp();
    void testRequestedSizeIsCappedAtTheProxyLimit();
    void testCdnHostUrlIsLeftAlone();

    void testDiscordAssetHosts();
    void testLoopingVideoLimitAllowsEitherOrientation();
};

void TestCdnUrls::testGifIsAnimatedWhateverItsFlags()
{
    QVERIFY(Cdn::isAnimatedImage(QUrl("https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=a&is=b&hm=c&"), false));
    QVERIFY(Cdn::isAnimatedImage(QUrl("https://cdn.discordapp.com/attachments/1/2/CAT.GIF"), false));
    QVERIFY(Cdn::isAnimatedImage(QUrl("https://example.com/a.gif#frag"), false));
    QVERIFY(!Cdn::isAnimatedImage(QUrl("https://cdn.discordapp.com/attachments/1/2/cat.png?ex=a"), false));
    QVERIFY(!Cdn::isAnimatedImage(QUrl("https://cdn.discordapp.com/attachments/1/2/cat.png?ex=a"), true));
}

void TestCdnUrls::testWebpAndAvifNeedTheAnimatedFlag()
{
    const QUrl webp("https://cdn.discordapp.com/attachments/1/2/cat.webp?ex=a&is=b&hm=c&");
    const QUrl avif("https://cdn.discordapp.com/attachments/1/2/cat.avif?ex=a&is=b&hm=c&");
    QVERIFY(!Cdn::isAnimatedImage(webp, false));
    QVERIFY(Cdn::isAnimatedImage(webp, true));
    QVERIFY(!Cdn::isAnimatedImage(avif, false));
    QVERIFY(Cdn::isAnimatedImage(avif, true));
}

void TestCdnUrls::testExtensionMustBeFollowedByEndQueryOrFragment()
{
    QVERIFY(!Cdn::isAnimatedImage(QUrl("https://example.com/a.gifv"), false));
    QVERIFY(!Cdn::isAnimatedImage(QUrl("https://example.com/a.gif.png"), false));
    QVERIFY(Cdn::isAnimatedImage(QUrl("https://example.com/a.gif.png?source=b.gif"), false));
}

void TestCdnUrls::testAnimatedGifUrlAsksOnlyForItsSize()
{
    const QUrl proxy("https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c");
    const QUrl animated = Cdn::animatedImageUrl(proxy, false, QSize(800, 600), QSize(400, 300));
    QCOMPARE(animated.toString(), QString("https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c&width=400&height=300"));
}

void TestCdnUrls::testAnimatedGifUrlAtItsOwnSizeIsTheProxyUrl()
{
    const QUrl proxy("https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c");
    QCOMPARE(Cdn::animatedImageUrl(proxy, false, QSize(320, 240), QSize(320, 240)), proxy);
}

void TestCdnUrls::testAnimatedWebpUrlAsksForAnimation()
{
    const QUrl proxy("https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c");
    QCOMPARE(Cdn::animatedImageUrl(proxy, true, QSize(800, 600), QSize(400, 300)).toString(),
             QString("https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c&animated=true&width=400&height=300"));
    QCOMPARE(Cdn::animatedImageUrl(proxy, false, QSize(800, 600), QSize(400, 300)).toString(),
             QString("https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c&width=400&height=300"));
}

void TestCdnUrls::testAnimatedAvifUrlAsksForAnimatedWebp()
{
    const QUrl proxy("https://media.discordapp.net/attachments/1/2/cat.avif?ex=a&is=b&hm=c");
    QCOMPARE(Cdn::animatedImageUrl(proxy, true, QSize(800, 600), QSize(400, 300)).toString(),
             QString("https://media.discordapp.net/attachments/1/2/cat.avif?ex=a&is=b&hm=c&animated=true&format=webp&width=400&height=300"));
}

void TestCdnUrls::testRequestedSizeIsCappedAtTheProxyLimit()
{
    const QUrl proxy("https://media.discordapp.net/attachments/1/2/wide.gif");
    QCOMPARE(Cdn::animatedImageUrl(proxy, false, QSize(9000, 100), QSize(8192, 1000)).toString(),
             QString("https://media.discordapp.net/attachments/1/2/wide.gif?width=4096&height=500"));
    QCOMPARE(Cdn::animatedImageUrl(proxy, false, QSize(100, 9000), QSize(1000, 8192)).toString(),
             QString("https://media.discordapp.net/attachments/1/2/wide.gif?width=500&height=4096"));
}

void TestCdnUrls::testCdnHostUrlIsLeftAlone()
{
    const QUrl cdn("https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=a&is=b&hm=c");
    QCOMPARE(Cdn::animatedImageUrl(cdn, false, QSize(800, 600), QSize(400, 300)), cdn);
}

void TestCdnUrls::testDiscordAssetHosts()
{
    QVERIFY(Cdn::isDiscordAssetUrl(QUrl("https://cdn.discordapp.com/attachments/1/2/cat.gif")));
    QVERIFY(Cdn::isDiscordAssetUrl(QUrl("https://media.discordapp.net/attachments/1/2/cat.gif")));
    QVERIFY(Cdn::isDiscordAssetUrl(QUrl("https://images-ext-1.discordapp.net/external/abc/https/media.tenor.com/x/y.mp4")));
    QVERIFY(!Cdn::isDiscordAssetUrl(QUrl("http://media.discordapp.net/attachments/1/2/cat.gif")));
    QVERIFY(!Cdn::isDiscordAssetUrl(QUrl("https://media.tenor.com/x/y.mp4")));
    QVERIFY(!Cdn::isDiscordAssetUrl(QUrl("https://discordapp.net.evil.example/x.mp4")));
    QVERIFY(!Cdn::isDiscordAssetUrl(QUrl("https://evildiscordapp.net/x.mp4")));
    QVERIFY(!Cdn::isDiscordAssetUrl(QUrl::fromLocalFile("C:/x.gif")));
}

void TestCdnUrls::testLoopingVideoLimitAllowsEitherOrientation()
{
    QVERIFY(Cdn::fitsLoopingVideoLimit(QSize(498, 280)));
    QVERIFY(Cdn::fitsLoopingVideoLimit(QSize(6016, 3384)));
    QVERIFY(Cdn::fitsLoopingVideoLimit(QSize(3384, 6016)));
    QVERIFY(!Cdn::fitsLoopingVideoLimit(QSize(6017, 3384)));
    QVERIFY(!Cdn::fitsLoopingVideoLimit(QSize(4000, 4000)));
}

QTEST_GUILESS_MAIN(TestCdnUrls)
#include "tst_CdnUrls.moc"
