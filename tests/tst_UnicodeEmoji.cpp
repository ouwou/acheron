#include "Core/Emoji/UnicodeEmojiIndex.hpp"

#include <QTest>

using namespace Acheron::Core;

class TestUnicodeEmoji : public QObject
{
    Q_OBJECT
private slots:
    void testByName();
    void testAliases();
    void testConvertSurrogateToBase();
    void testTranslateNamesToSurrogates();
    void testTopLevel();
};

// U+1F44D U+1F3FC — thumbsup with the medium-light skin tone.
static const QString thumbsUpTone2 = QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC");

void TestUnicodeEmoji::testByName()
{
    const UnicodeEmoji *emoji = UnicodeEmojiIndex::instance().byName("thumbsup::skin-tone-2");
    QVERIFY(emoji != nullptr);
    QCOMPARE(emoji->surrogates, thumbsUpTone2);
    QVERIFY(emoji->isDiversityChild);

    // The parent is reachable under each of its aliases.
    const UnicodeEmoji *parent = UnicodeEmojiIndex::instance().byName("thumbsup");
    QVERIFY(parent != nullptr);
    QCOMPARE(UnicodeEmojiIndex::instance().byName("+1"), parent);
    QVERIFY(parent->hasDiversity);
    QCOMPARE(UnicodeEmojiIndex::primaryName(*parent), QStringLiteral("thumbsup"));

    QVERIFY(UnicodeEmojiIndex::instance().byName("definitely_not_an_emoji") == nullptr);
}

void TestUnicodeEmoji::testAliases()
{
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    QVERIFY(index.byName("laughing") != nullptr);
    QCOMPARE(index.byName("satisfied"), index.byName("laughing"));

    // Keywords widen search but are not names.
    QVERIFY(index.byName("grinning")->keywords.contains("happy"));
    QVERIFY(index.byName("happy") == nullptr);
}

void TestUnicodeEmoji::testConvertSurrogateToBase()
{
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    QCOMPARE(index.convertSurrogateToBase(thumbsUpTone2), index.byName("thumbsup"));
    QCOMPARE(index.bySurrogate(thumbsUpTone2), index.byName("thumbsup::skin-tone-2"));
}

void TestUnicodeEmoji::testTranslateNamesToSurrogates()
{
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();

    // Unknown names and custom emoji tags are left alone.
    QCOMPARE(index.translateNamesToSurrogates(":thumbsup: hi <:pepe:123> :nope:"),
             QString::fromUtf8("\xF0\x9F\x91\x8D") + QStringLiteral(" hi <:pepe:123> :nope:"));

    QCOMPARE(index.translateNamesToSurrogates(":thumbsup::skin-tone-2:"), thumbsUpTone2);
    QCOMPARE(index.translateNamesToSurrogates("<a:pepe:123>"), QStringLiteral("<a:pepe:123>"));
    QCOMPARE(index.translateNamesToSurrogates("no emoji here"), QStringLiteral("no emoji here"));

    // A custom emoji may be named after a unicode one; the tag must survive intact.
    QCOMPARE(index.translateNamesToSurrogates("<:100:123456789>"), QStringLiteral("<:100:123456789>"));
    QCOMPARE(index.translateNamesToSurrogates(":100: <:100:123456789>"),
             QString::fromUtf8("\xF0\x9F\x92\xAF") + QStringLiteral(" <:100:123456789>"));
}

void TestUnicodeEmoji::testTopLevel()
{
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    QVERIFY(!index.topLevel().isEmpty());
    for (const UnicodeEmoji *emoji : index.topLevel())
        QVERIFY(!emoji->isDiversityChild);
}

QTEST_GUILESS_MAIN(TestUnicodeEmoji)
#include "tst_UnicodeEmoji.moc"
