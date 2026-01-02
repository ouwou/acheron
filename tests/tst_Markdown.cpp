#include "Core/Markdown/Parser.hpp"

#include <QElapsedTimer>
#include <QTest>

using namespace Acheron::Core::Markdown;

class TestMarkdown : public QObject
{
    Q_OBJECT
private slots:
    void testFoo();
};

void TestMarkdown::testFoo()
{
    Parser parser;
    
    return;
}

QTEST_MAIN(TestMarkdown)
#include "tst_Markdown.moc"
