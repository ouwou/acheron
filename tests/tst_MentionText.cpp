#include "UI/Input/MentionText.hpp"

#include <QGuiApplication>
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
    void testRawMentionsRoundTrip();
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

void TestMentionText::testRawMentionsRoundTrip()
{
    const QString raw = QStringLiteral("hi <@!123> and <@&7>\nin <#456>, <@999> <:pepe:1>");
    QTextDocument document;
    QTextCursor cursor(&document);
    insertWithMentions(cursor, raw, [](QChar kind, Acheron::Core::Snowflake id) -> QString {
        if (kind == '@' && id == Acheron::Core::Snowflake(123))
            return QStringLiteral("@bob");
        if (kind == '&' && id == Acheron::Core::Snowflake(7))
            return QStringLiteral("@mods");
        if (kind == '#' && id == Acheron::Core::Snowflake(456))
            return QStringLiteral("#general");
        return {};
    }, {});

    QCOMPARE(document.toPlainText(), QStringLiteral("hi @bob and @mods\nin #general, <@999> <:pepe:1>"));
    QCOMPARE(toMessageText(&document), raw);
}

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    TestMentionText test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_MentionText.moc"
