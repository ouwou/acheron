#include "UI/Input/MentionText.hpp"

#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

using namespace Acheron::UI;

class TestMentionText : public QObject
{
    Q_OBJECT
private slots:
    void testMentionsBecomeRaw();
    void testEditedMentionStaysPlain();
};

void TestMentionText::testMentionsBecomeRaw()
{
    QTextDocument document;
    QTextCursor cursor(&document);
    cursor.insertText(QStringLiteral("hi "));
    insertMention(cursor, QStringLiteral("@bob"), QStringLiteral("<@123>"), {});
    cursor.insertText(QStringLiteral("see "));
    insertMention(cursor, QStringLiteral("#general"), QStringLiteral("<#456>"), {});
    cursor.insertText(QString(QChar::LineSeparator) + QStringLiteral("@bob"));
    cursor.insertBlock();
    insertMention(cursor, QStringLiteral("@bob"), QStringLiteral("<@123>"), {});

    QCOMPARE(toMessageText(&document), QStringLiteral("hi <@123> see <#456> \n@bob\n<@123> "));
}

void TestMentionText::testEditedMentionStaysPlain()
{
    QTextDocument document;
    QTextCursor cursor(&document);
    insertMention(cursor, QStringLiteral("@bob"), QStringLiteral("<@123>"), {});
    cursor.setPosition(2);
    cursor.insertText(QStringLiteral("x"));

    QCOMPARE(toMessageText(&document), QStringLiteral("@bxob "));
}

QTEST_MAIN(TestMentionText)
#include "tst_MentionText.moc"
