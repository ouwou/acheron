#include "Core/Markdown/Parser.hpp"

#include <QElapsedTimer>
#include <QTest>

using namespace Acheron::Core::Markdown;

class TestMarkdown : public QObject
{
    Q_OBJECT
private slots:
    void testNewlines();
};

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

QTEST_MAIN(TestMarkdown)
#include "tst_Markdown.moc"
