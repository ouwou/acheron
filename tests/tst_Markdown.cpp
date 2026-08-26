#include "Core/EmojiSegmenter.hpp"
#include "Core/Markdown/Parser.hpp"

#include <QElapsedTimer>
#include <QTest>

using namespace Acheron::Core::Markdown;

class TestMarkdown : public QObject
{
    Q_OBJECT
private slots:
    void testNewlines();
    void testAutolink();
    void testUrl();
    void testEmojiSequences();
};

static QString renderInline(const QString &source)
{
    Parser parser;
    ParseState state;
    state.isInline = true;
    auto nodes = parser.parse(source, state);
    return parser.toHtml(nodes);
}

void TestMarkdown::testNewlines()
{
    Parser parser;

    ParseState state;
    state.isInline = true;
    auto nodes = parser.parse("i\nlove\n\ncats", state);
    auto html = parser.toHtml(nodes);

    QVERIFY(html == "i<br>love<br><br>cats");

    return;
}

void TestMarkdown::testAutolink()
{
    // `<url>` (embed suppression): the angle brackets are stripped from both the visible text and
    // the href. Regression test for the trailing `>` leaking into the opened URL. Note the "/" —
    // a host-only http(s) URL is normalized to have a "/" path, matching Discord.
    QCOMPARE(renderInline("<https://example.com>"),
             QStringLiteral("<a href=\"https://example.com/\">https://example.com/</a>"));

    // Autolink embedded in surrounding text.
    QCOMPARE(renderInline("a <https://example.com> b"),
             QStringLiteral("a <a href=\"https://example.com/\">https://example.com/</a> b"));

    // A bare url (no brackets) is normalized the same way.
    QCOMPARE(renderInline("https://example.com"),
             QStringLiteral("<a href=\"https://example.com/\">https://example.com/</a>"));

    // A url that already has a path is left untouched.
    QCOMPARE(renderInline("<https://example.com/a/b>"),
             QStringLiteral("<a href=\"https://example.com/a/b\">https://example.com/a/b</a>"));

    // Non-special scheme (discord): a valid link, but WHATWG does NOT add a "/" path to it.
    QCOMPARE(renderInline("<discord://foo>"),
             QStringLiteral("<a href=\"discord://foo\">discord://foo</a>"));

    // Disallowed scheme: brackets are still removed, but it renders as plain text, not a link.
    QCOMPARE(renderInline("<ftp://example.com>"), QStringLiteral("ftp://example.com"));

    // http/https with no host is not a valid link target -> plain text.
    QCOMPARE(renderInline("<https://>"), QStringLiteral("https://"));
}

void TestMarkdown::testUrl()
{
    // Balanced parentheses inside a URL are kept (regression: the closing paren used to be dropped
    // from Wikipedia-style links). Mirrors Discord's `url` rule.
    QCOMPARE(
            renderInline("https://en.wikipedia.org/wiki/Django_(web_framework)"),
            QStringLiteral("<a href=\"https://en.wikipedia.org/wiki/Django_(web_framework)\">"
                           "https://en.wikipedia.org/wiki/Django_(web_framework)</a>"));

    // A URL wrapped in parens links only the URL; the unbalanced trailing `)` stays as text.
    // (The linked URL is then normalized to a "/" path.)
    QCOMPARE(renderInline("(https://example.com)"),
             QStringLiteral("(<a href=\"https://example.com/\">https://example.com/</a>)"));

    // Only a single unbalanced trailing `)` is trimmed, matching Discord's scan exactly.
    QCOMPARE(renderInline("https://example.com))"),
             QStringLiteral("<a href=\"https://example.com)/\">https://example.com)/</a>)"));

    // A host-only URL with a non-default port still gets the "/" path.
    QCOMPARE(renderInline("https://localhost:3000"),
             QStringLiteral("<a href=\"https://localhost:3000/\">https://localhost:3000/</a>"));
}

void TestMarkdown::testEmojiSequences()
{
    using Acheron::Core::countUnicodeEmojisSegmented;
    using Acheron::Core::extractEmojiSequences;

    const QString thumbsUp = QString::fromUtf8("\xF0\x9F\x91\x8D");
    const QString thumbsUpTone2 = QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC");
    const QString family = QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7");
    const QString keycapOne = QString::fromUtf8("1\xEF\xB8\x8F\xE2\x83\xA3");

    // Tone modifiers, ZWJ chains and keycaps each come back as one intact sequence.
    QCOMPARE(extractEmojiSequences("hi " + thumbsUpTone2 + " and " + family + keycapOne + "!"),
             QStringList({ thumbsUpTone2, family, keycapOne }));
    QVERIFY(extractEmojiSequences("plain text <:pepe:123> 1 + 1").isEmpty());

    QCOMPARE(countUnicodeEmojisSegmented(thumbsUp + " " + family), 2);
    QCOMPARE(countUnicodeEmojisSegmented("x " + thumbsUp), -1);
    QCOMPARE(countUnicodeEmojisSegmented("   "), 0);
}

QTEST_MAIN(TestMarkdown)
#include "tst_Markdown.moc"
