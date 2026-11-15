#include "Core/MentionSearch.hpp"

#include <QTest>

using namespace Acheron::Core::MentionSearch;

class TestMentionSearch : public QObject
{
    Q_OBJECT
private slots:
    void testFolding();
    void testFuzzy();
    void testUserPrefixKeepsOrder();
    void testUserLookalikesAndFuzzyFill();
    void testUserFilterAndLimit();
    void testRoleRanking();
    void testChannelRanking();
    void testChannelContextTokens();
    void testThreadPenalties();
};

static QList<UserCandidate> users(const QStringList &usernames)
{
    QList<UserCandidate> list;
    for (const QString &name : usernames)
        list.append({ name, QString(), QString() });
    return list;
}

void TestMentionSearch::testFolding()
{
    QCOMPARE(stripDiacritics(QStringLiteral("Café")), QStringLiteral("Cafe"));
    // a Cyrillic a and a mathematical bold a fold like the Latin letter (the table also turns m into rn)
    QCOMPARE(foldConfusables(QStringLiteral("аdmin")), foldConfusables(QStringLiteral("admin")));
    QCOMPARE(foldConfusables(QStringLiteral("\U0001D41Ab")), foldConfusables(QStringLiteral("ab")));
}

void TestMentionSearch::testFuzzy()
{
    QVERIFY(isSubsequence(QStringLiteral("adm"), QStringLiteral("a_d_m")));
    QVERIFY(!isSubsequence(QStringLiteral("ma"), QStringLiteral("am")));
    QVERIFY(!isSubsequence(QStringLiteral("abc"), QStringLiteral("abd")));
    QVERIFY(!isSubsequence(QStringLiteral("abcd"), QStringLiteral("abc")));
    QVERIFY(isSubsequence(QStringLiteral("abc"), QStringLiteral("abc")));
    QVERIFY(isSubsequence(QString(), QStringLiteral("x")));
}

void TestMentionSearch::testUserPrefixKeepsOrder()
{
    QList<UserCandidate> list = users({ "zed", "alice", "bob", "xavier" });
    list[2].nick = QStringLiteral("Alfred");
    list[3].globalName = QStringLiteral("Alan");
    QCOMPARE(rankUsers(QStringLiteral("Al"), list), (QList<int>{ 1, 2, 3 }));
}

void TestMentionSearch::testUserLookalikesAndFuzzyFill()
{
    // prefix matches first, then look-alike prefixes, then subsequence matches to fill the rest
    const QList<UserCandidate> list = users({ "xalien", "аlex", "anle", "alma" });
    QCOMPARE(rankUsers(QStringLiteral("al"), list), (QList<int>{ 3, 1, 0, 2 }));
}

void TestMentionSearch::testUserFilterAndLimit()
{
    QStringList names;
    for (int i = 0; i < 12; ++i)
        names << QStringLiteral("user%1").arg(i);
    const QList<UserCandidate> list = users(names);
    QCOMPARE(rankUsers(QStringLiteral("user"), list).size(), ResultLimit);
    const QList<int> odd = rankUsers(QStringLiteral("user"), list, [](int index) { return index % 2 == 1; });
    QCOMPARE(odd, (QList<int>{ 1, 3, 5, 7, 9, 11 }));
}

void TestMentionSearch::testRoleRanking()
{
    QCOMPARE(matchSorterRank(QStringLiteral("Mods"), QStringLiteral("Mods")), 7.0);
    QCOMPARE(matchSorterRank(QStringLiteral("Mods"), QStringLiteral("mods")), 6.0);
    QCOMPARE(matchSorterRank(QStringLiteral("Moderators"), QStringLiteral("mod")), 5.0);
    QCOMPARE(matchSorterRank(QStringLiteral("Senior Mods"), QStringLiteral("mod")), 4.0);
    QCOMPARE(matchSorterRank(QStringLiteral("Admods"), QStringLiteral("mod")), 3.0);
    QCOMPARE(matchSorterRank(QStringLiteral("Cool Kids Club"), QStringLiteral("ckc")), 2.0);
    const double fuzzy = matchSorterRank(QStringLiteral("moderator"), QStringLiteral("mdr"));
    QVERIFY(fuzzy > 1 && fuzzy < 2);
    QCOMPARE(matchSorterRank(QStringLiteral("abc"), QStringLiteral("x")), 0.0);

    // equal ranks fall back to alphabetical order
    const QStringList names = { "Senior Mods", "mods", "Moderators", "Beta Mods" };
    QCOMPARE(rankByName(names, QStringLiteral("mods")), (QList<int>{ 1, 3, 0, 2 }));
}

void TestMentionSearch::testChannelRanking()
{
    QList<ChannelCandidate> list;
    for (const char *name : { "off-topic", "general", "gen-chat", "lounge" }) {
        ChannelCandidate candidate;
        candidate.name = QString::fromLatin1(name);
        list.append(candidate);
    }
    list[3].isVoice = true;

    QCOMPARE(rankChannels(QStringLiteral("gen"), list, false), (QList<int>{ 1, 2 }));
    // the whole query matches a hyphen in place of its space
    QCOMPARE(rankChannels(QStringLiteral("off topic"), list, false), (QList<int>{ 0 }));
    // an empty query lists every channel, a voice channel scoring one lower among text channels
    QCOMPARE(rankChannels(QString(), list, false), (QList<int>{ 0, 1, 2, 3 }));
    QCOMPARE(rankChannels(QStringLiteral("lounge"), list, true), (QList<int>{ 3 }));
}

void TestMentionSearch::testChannelContextTokens()
{
    ChannelCandidate memes;
    memes.name = QStringLiteral("general");
    memes.parentName = QStringLiteral("memes");
    ChannelCandidate other;
    other.name = QStringLiteral("general");
    other.parentName = QStringLiteral("staff");
    // the second word has to match the category or server name
    QCOMPARE(rankChannels(QStringLiteral("general memes"), { other, memes }, false), (QList<int>{ 1 }));
}

void TestMentionSearch::testThreadPenalties()
{
    ChannelCandidate joined;
    joined.name = QStringLiteral("release notes");
    joined.isThread = joined.isActiveThread = joined.isJoinedThread = true;
    ChannelCandidate notJoined = joined;
    notJoined.isJoinedThread = false;
    ChannelCandidate text;
    text.name = QStringLiteral("release-planning");
    QCOMPARE(rankChannels(QStringLiteral("release"), { notJoined, text, joined }, false), (QList<int>{ 1, 2, 0 }));
}

QTEST_MAIN(TestMentionSearch)
#include "tst_MentionSearch.moc"
