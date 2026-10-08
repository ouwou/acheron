#include "Core/Gifs/FavoriteGifRules.hpp"
#include "UI/Gifs/GifMasonryLayout.hpp"

#include <QTest>

using namespace Acheron;
using namespace Acheron::Core;
using Acheron::Proto::FavoriteGif;
using Acheron::Proto::GifType;

class TestFavoriteGifs : public QObject
{
    Q_OBJECT
private slots:
    void testKeyDropsTheSignatureOfDiscordAttachmentsOnly();

    void testGifvEmbedIsStoredAsItsProxiedVideo();
    void testGifAttachmentIsStoredAsAnimatedWebp();
    void testWebpAttachmentOnlyGainsTheAnimatedFlag();
    void testKlipyEmbedIsStoredAsItsThumbnail();
    void testProviderResultPrefersItsGifSource();
    void testProtocolRelativeSourceGainsHttps();

    void testOrderFollowsTheHighestExisting();
    void testNewestComesFirst();
    void testSearchIgnoresCaseDashesUnderscoresAndSpaces();

    void testOnlyDiscordHostedSourcesAreLoaded();
    void testTileUrlsForAGifSource();
    void testTileUrlsForAVideoSource();

    void testGutterTightensAsColumnsAreAdded();
    void testTilesFillTheShortestColumn();
    void testDenseGridFitsMoreColumnsInTheSameWidth();
    void testStillsComeInTwoSizesWhateverTheDensity();
    void testTileHeightKeepsTheAspectRatio();
};

void TestFavoriteGifs::testKeyDropsTheSignatureOfDiscordAttachmentsOnly()
{
    QCOMPARE(FavoriteGifRules::keyFor("https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&"), QString("https://cdn.discordapp.com/attachments/1/2/cat.gif"));
    QCOMPARE(FavoriteGifRules::keyFor("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&width=10&is=bb&hm=cc"), QString("https://media.discordapp.net/attachments/1/2/cat.gif?width=10"));
    QCOMPARE(FavoriteGifRules::keyFor("https://tenor.com/view/cat-123?ex=aa"), QString("https://tenor.com/view/cat-123?ex=aa"));
    QCOMPARE(FavoriteGifRules::keyFor("https://images-ext-1.discordapp.net/external/sig/https/x.test/a.gif?ex=aa"), QString("https://images-ext-1.discordapp.net/external/sig/https/x.test/a.gif?ex=aa"));
    QCOMPARE(FavoriteGifRules::keyFor("not a url"), QString("not a url"));
}

void TestFavoriteGifs::testGifvEmbedIsStoredAsItsProxiedVideo()
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "https://images-ext-1.discordapp.net/external/sig/https/media.tenor.com/abc/cat.mp4", {}, GifType::Video, QSize(498, 280) };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 7);

    QCOMPARE(gif.url, QString("https://tenor.com/view/cat-123"));
    QCOMPARE(gif.src, candidate.src);
    QCOMPARE(gif.format, GifType::Video);
    QCOMPARE(gif.width, 498u);
    QCOMPARE(gif.height, 280u);
    QCOMPARE(gif.order, 7u);
    QVERIFY(gif.messageAsReceived.isEmpty());
}

void TestFavoriteGifs::testGifAttachmentIsStoredAsAnimatedWebp()
{
    const FavoriteGifCandidate candidate{ "https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&", "https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&", {}, GifType::Image, QSize(320, 240) };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    QCOMPARE(gif.url, QString("https://cdn.discordapp.com/attachments/1/2/cat.gif"));
    QCOMPARE(gif.src, QString("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&format=webp&animated=true"));
    QCOMPARE(gif.format, GifType::Image);
}

void TestFavoriteGifs::testWebpAttachmentOnlyGainsTheAnimatedFlag()
{
    const FavoriteGifCandidate candidate{ "https://cdn.discordapp.com/attachments/1/2/cat.webp", "https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa", {}, GifType::Image, QSize(320, 240) };
    QCOMPARE(FavoriteGifRules::stored(candidate, 1).src, QString("https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa&animated=true"));
}

void TestFavoriteGifs::testKlipyEmbedIsStoredAsItsThumbnail()
{
    const FavoriteGifCandidate candidate{ "https://klipy.com/gifs/cat", "https://images-ext-1.discordapp.net/external/sig/https/static.klipy.com/cat.mp4",
                                          "https://images-ext-1.discordapp.net/external/sig2/https/static.klipy.com/cat.webp", GifType::Video, QSize(400, 300) };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    QCOMPARE(gif.src, QString("https://images-ext-1.discordapp.net/external/sig2/https/static.klipy.com/cat.webp?animated=true"));
    QCOMPARE(gif.format, GifType::Image);
}

void TestFavoriteGifs::testProviderResultPrefersItsGifSource()
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "//media.tenor.com/abc/cat.webm", "//media.tenor.com/abc/cat.gif", GifType::Video, QSize(400, 300) };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    QCOMPARE(gif.src, QString("https://media.tenor.com/abc/cat.gif"));
    QCOMPARE(gif.format, GifType::Image);
}

void TestFavoriteGifs::testProtocolRelativeSourceGainsHttps()
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "//media.tenor.com/abc/cat.mp4", {}, GifType::Video, QSize(400, 300) };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    QCOMPARE(gif.src, QString("https://media.tenor.com/abc/cat.mp4"));
    QCOMPARE(gif.format, GifType::Video);
}

void TestFavoriteGifs::testOrderFollowsTheHighestExisting()
{
    QCOMPARE(FavoriteGifRules::orderAfter({}), 1u);

    FavoriteGif low;
    low.order = 3;
    FavoriteGif high;
    high.order = 41;
    QCOMPARE(FavoriteGifRules::orderAfter({ high, low }), 42u);
}

void TestFavoriteGifs::testNewestComesFirst()
{
    FavoriteGif first;
    first.url = "first";
    first.order = 1;
    FavoriteGif second;
    second.url = "second";
    second.order = 2;
    FavoriteGif third;
    third.url = "third";
    third.order = 3;

    const QList<FavoriteGif> sorted = FavoriteGifRules::newestFirst({ second, third, first });
    QCOMPARE(sorted[0].url, QString("third"));
    QCOMPARE(sorted[1].url, QString("second"));
    QCOMPARE(sorted[2].url, QString("first"));
}

void TestFavoriteGifs::testSearchIgnoresCaseDashesUnderscoresAndSpaces()
{
    const QString url = "https://tenor.com/view/Happy-Cat_dance-123";
    QVERIFY(FavoriteGifRules::matchesSearch(url, "happy cat"));
    QVERIFY(FavoriteGifRules::matchesSearch(url, "CAT_DANCE"));
    QVERIFY(FavoriteGifRules::matchesSearch(url, "catdance"));
    QVERIFY(!FavoriteGifRules::matchesSearch(url, "dog"));
}

void TestFavoriteGifs::testOnlyDiscordHostedSourcesAreLoaded()
{
    FavoriteGif direct;
    direct.src = "https://media.tenor.com/abc/cat.mp4";
    direct.format = GifType::Video;
    QVERIFY(FavoriteGifRules::clipServedByDiscord(direct).isEmpty());
    QVERIFY(FavoriteGifRules::stillServedByDiscord(direct).isEmpty());

    FavoriteGif insecure;
    insecure.src = "http://media.discordapp.net/attachments/1/2/cat.gif";
    QVERIFY(FavoriteGifRules::clipServedByDiscord(insecure).isEmpty());
}

void TestFavoriteGifs::testTileUrlsForAGifSource()
{
    FavoriteGif gif;
    gif.format = GifType::Image;

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&format=webp&animated=true";
    QCOMPARE(FavoriteGifRules::clipServedByDiscord(gif).toString(), QString("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&format=webp&animated=true"));
    QCOMPARE(FavoriteGifRules::stillServedByDiscord(gif).toString(), QString("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa"));

    gif.src = "https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif";
    QCOMPARE(FavoriteGifRules::clipServedByDiscord(gif).toString(), QString("https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif?format=webp&animated=true"));
    QCOMPARE(FavoriteGifRules::stillServedByDiscord(gif).toString(), QString("https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif"));

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.avif?ex=aa";
    QCOMPARE(FavoriteGifRules::clipServedByDiscord(gif).toString(), QString("https://media.discordapp.net/attachments/1/2/cat.avif?ex=aa&format=webp&animated=true"));

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa";
    QCOMPARE(FavoriteGifRules::clipServedByDiscord(gif).toString(), QString("https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa&animated=true"));
}

void TestFavoriteGifs::testTileUrlsForAVideoSource()
{
    FavoriteGif gif;
    gif.format = GifType::Video;
    gif.src = "https://images-ext-1.discordapp.net/external/sig/https/media.tenor.com/abc/cat.mp4";

    QCOMPARE(FavoriteGifRules::clipServedByDiscord(gif).toString(), gif.src);
    QCOMPARE(FavoriteGifRules::stillServedByDiscord(gif).toString(), gif.src);
}

void TestFavoriteGifs::testGutterTightensAsColumnsAreAdded()
{
    using namespace UI::GifMasonry;
    QCOMPARE(gutterFor(1), RoomiestGutter);
    QCOMPARE(gutterFor(2), 12);
    QCOMPARE(gutterFor(3), 8);
    QCOMPARE(gutterFor(4), 6);
    QCOMPARE(gutterFor(5), TightestGutter);
    QCOMPARE(gutterFor(MostColumns), TightestGutter);
}

void TestFavoriteGifs::testDenseGridFitsMoreColumnsInTheSameWidth()
{
    using namespace UI::GifMasonry;
    QList<QSize> sevenSquares;
    for (int i = 0; i < 7; ++i)
        sevenSquares.append(QSize(100, 100));
    const Arrangement arrangement = arrange(sevenSquares, 498, MostColumns);

    const int columnWidth = 78;
    QCOMPARE(arrangement.gutter, TightestGutter);
    for (int column = 0; column < MostColumns; ++column)
        QCOMPARE(arrangement.tiles[column], QRect(TightestGutter + column * (columnWidth + TightestGutter), TightestGutter, columnWidth, columnWidth));
    QCOMPARE(arrangement.tiles[6], QRect(TightestGutter, TightestGutter + columnWidth + TightestGutter, columnWidth, columnWidth));
    QVERIFY(arrangement.tiles[5].right() < 498);
}

void TestFavoriteGifs::testTilesFillTheShortestColumn()
{
    using namespace UI::GifMasonry;
    const Arrangement arrangement = arrange({ QSize(100, 200), QSize(100, 50), QSize(100, 50), QSize(100, 100) }, 436, 2);

    const int Gutter = RoomiestGutter;
    const int columnWidth = 200;
    QCOMPARE(arrangement.tiles.size(), 4);
    QCOMPARE(arrangement.tiles[0], QRect(Gutter, Gutter, columnWidth, 400));
    QCOMPARE(arrangement.tiles[1], QRect(Gutter + columnWidth + Gutter, Gutter, columnWidth, 100));
    QCOMPARE(arrangement.tiles[2], QRect(Gutter + columnWidth + Gutter, Gutter + 100 + Gutter, columnWidth, 100));
    QCOMPARE(arrangement.tiles[3], QRect(Gutter + columnWidth + Gutter, Gutter + 2 * (100 + Gutter), columnWidth, 200));
    QCOMPARE(arrangement.contentHeight, Gutter + 2 * (100 + Gutter) + 200 + Gutter);
}

void TestFavoriteGifs::testTileHeightKeepsTheAspectRatio()
{
    using namespace UI::GifMasonry;
    const Arrangement arrangement = arrange({ QSize(498, 280), QSize(0, 0) }, 236, 1);

    const int Gutter = RoomiestGutter;
    QCOMPARE(arrangement.tiles[0], QRect(Gutter, Gutter, 212, 119));
    QCOMPARE(arrangement.tiles[1].size(), QSize(212, 212));
    QCOMPARE(arrange({}, 236, 1).contentHeight, Gutter);
}

void TestFavoriteGifs::testStillsComeInTwoSizesWhateverTheDensity()
{
    using namespace UI::GifMasonry;
    const QSize natural(498, 280);
    const int viewportWidth = 498;

    QList<QSize> sizesAskedFor;
    for (int columns = FewestColumns; columns <= MostColumns; ++columns) {
        const int tileWidth = arrange({ natural }, viewportWidth, columns).tiles[0].width();
        const QSize still = stillSizeFor(natural, tileWidth);
        QVERIFY(still.width() >= tileWidth);
        if (!sizesAskedFor.contains(still))
            sizesAskedFor.append(still);
    }

    QCOMPARE(sizesAskedFor, (QList<QSize>{ QSize(RoomyStillWidth, 144), QSize(DenseStillWidth, 72) }));
    QCOMPARE(stillSizeFor(QSize(0, 0), 100), QSize(DenseStillWidth, DenseStillWidth));
}

QTEST_GUILESS_MAIN(TestFavoriteGifs)
#include "tst_FavoriteGifs.moc"
