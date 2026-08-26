// Tests for Core::FrecencyTracker, our port of Discord's frecency ranking. Expectations here are
// hand-computed from the client's formulas (weight buckets, ceil(totalUses * score / samples),
// the reaction variant, and the 100-entry persistence cap) so a drift in ranking shows up as a
// failing number, not as "emoji come out in a different order than the official client".

#include "Core/Emoji/FrecencyTracker.hpp"

#include <QTest>

using namespace Acheron::Core;

static constexpr uint64_t DAY = 86400000ULL;
static constexpr uint64_t NOW = 1700000000000ULL;

class TestFrecency : public QObject
{
    Q_OBJECT
private slots:
    void testWeightBuckets();
    void testAgeInDays();
    void testTrack();
    void testCompute();
    void testOldEntrySurvives();
    void testEmptyRecentUsesDropped();
    void testFrequentlyOrderingAndCap();
    void testComputeSkipsCleanEntries();
    void testReactionFormula();
    void testCapForPersistence();
    void testOverwriteHistory();
};

static FrecencyTracker::Entry makeEntry(uint32_t totalUses, const QList<uint64_t> &recentUses)
{
    FrecencyTracker::Entry entry;
    entry.totalUses = totalUses;
    entry.recentUses = recentUses;
    return entry;
}

void TestFrecency::testWeightBuckets()
{
    // Boundaries of <=3 / <=15 / <=30 / <=45 / <=80 / else.
    QCOMPARE(FrecencyTracker::defaultComputeWeight(0), 100);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(3), 100);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(4), 70);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(15), 70);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(16), 50);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(30), 50);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(31), 30);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(45), 30);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(46), 10);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(80), 10);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(81), 1);
    QCOMPARE(FrecencyTracker::defaultComputeWeight(10000), 1);

    // A timestamp in the future gives a negative age, which lands in the freshest bucket just as
    // moment's truncating diff does.
    QCOMPARE(FrecencyTracker::defaultComputeWeight(-1), 100);
}

void TestFrecency::testAgeInDays()
{
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW), 0);
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW - DAY), 1);
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW - (DAY - 1)), 0);
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW - (3 * DAY + 1)), 3);
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW - (4 * DAY - 1)), 3);
    // Truncation toward zero, not floor.
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW + DAY), -1);
    QCOMPARE(FrecencyTracker::ageInDays(NOW, NOW + DAY / 2), 0);
}

void TestFrecency::testTrack()
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());
    QVERIFY(!tracker.isDirty());

    tracker.track("thumbsup", NOW - 5 * DAY);
    QVERIFY(tracker.isDirty());
    QCOMPARE(tracker.usageHistory().size(), 1);

    const FrecencyTracker::Entry &fresh = tracker.usageHistory().value("thumbsup");
    QCOMPARE(fresh.totalUses, 1u);
    QCOMPARE(fresh.recentUses, QList<uint64_t>({ NOW - 5 * DAY }));
    QCOMPARE(fresh.frecency, -1);
    QCOMPARE(fresh.score, 0.0);

    // An existing entry accumulates uses; an explicit timestamp older than what is already there
    // is sorted back into place.
    tracker.track("thumbsup", NOW - 9 * DAY, 3);
    const FrecencyTracker::Entry &merged = tracker.usageHistory().value("thumbsup");
    QCOMPARE(merged.totalUses, 4u);
    QCOMPARE(merged.recentUses, QList<uint64_t>({ NOW - 9 * DAY, NOW - 5 * DAY }));
    QCOMPARE(merged.frecency, -1);

    // Only the newest maxSamples (10) timestamps are kept; the oldest are shifted off the front.
    FrecencyTracker capped;
    for (int i = 0; i < 12; ++i)
        capped.track("eyes", NOW - static_cast<uint64_t>(12 - i) * DAY);

    const FrecencyTracker::Entry &shifted = capped.usageHistory().value("eyes");
    QCOMPARE(shifted.totalUses, 12u);
    QCOMPARE(shifted.recentUses.size(), 10);
    QCOMPARE(shifted.recentUses.first(), NOW - 10 * DAY);
    QCOMPARE(shifted.recentUses.last(), NOW - DAY);
}

void TestFrecency::testCompute()
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("thumbsup", makeEntry(5, { NOW - 40 * DAY, NOW - 10 * DAY, NOW - DAY }));
    tracker.overwriteHistory(history);

    tracker.compute(NOW);
    QVERIFY(!tracker.isDirty());

    // weight(40) + weight(10) + weight(1) = 30 + 70 + 100
    QCOMPARE(tracker.score("thumbsup").value(), 200.0);
    // ceil(5 * (200 / 3)) = ceil(333.33) = 334
    QCOMPARE(tracker.frecency("thumbsup").value(), 334);

    QVERIFY(!tracker.score("nope").has_value());
    QVERIFY(!tracker.frecency("nope").has_value());
}

void TestFrecency::testOldEntrySurvives()
{
    // Age alone never drops an entry: past 80 days the weight bottoms out at 1, which is still a
    // positive score.
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("ancient", makeEntry(1, { NOW - 400 * DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    QCOMPARE(tracker.usageHistory().size(), 1);
    QCOMPARE(tracker.score("ancient").value(), 1.0);
    QCOMPARE(tracker.frecency("ancient").value(), 1);
}

void TestFrecency::testEmptyRecentUsesDropped()
{
    // Score 0 is only reachable with no samples at all, and that entry is deleted.
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("empty", makeEntry(9, {}));
    history.insert("used", makeEntry(1, { NOW - DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    QCOMPARE(tracker.usageHistory().size(), 1);
    QVERIFY(!tracker.score("empty").has_value());
    QCOMPARE(tracker.frequently(NOW), QStringList({ "used" }));
}

void TestFrecency::testFrequentlyOrderingAndCap()
{
    FrecencyTracker tracker(FrecencyTracker::chatEmojiOptions());

    // 50 entries, all used once a day ago (score 100), so frecency == totalUses * 100 and the
    // ordering is exactly the reverse of the index.
    QHash<QString, FrecencyTracker::Entry> history;
    for (int i = 0; i < 50; ++i) {
        history.insert(QStringLiteral("k%1").arg(i, 2, 10, QLatin1Char('0')),
                       makeEntry(static_cast<uint32_t>(i + 1), { NOW - DAY }));
    }
    tracker.overwriteHistory(history);

    QVERIFY(tracker.isDirty());
    const QStringList frequently = tracker.frequently(NOW);
    QVERIFY(!tracker.isDirty());

    QCOMPARE(frequently.size(), 42);
    QCOMPARE(frequently.first(), QStringLiteral("k49"));
    QCOMPARE(frequently.at(1), QStringLiteral("k48"));
    QCOMPARE(frequently.last(), QStringLiteral("k08"));
    QCOMPARE(tracker.frecency("k49").value(), 5000);

    // Entries below the cut are still tracked, just not "frequently".
    QCOMPARE(tracker.usageHistory().size(), 50);
    QCOMPARE(tracker.frecency("k00").value(), 100);
}

void TestFrecency::testComputeSkipsCleanEntries()
{
    // frecency != -1 means "already computed"; a later compute() must leave it alone, exactly like
    // the client (only track/overwriteHistory invalidate).
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("thumbsup", makeEntry(2, { NOW - DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);
    QCOMPARE(tracker.frecency("thumbsup").value(), 200);

    tracker.compute(NOW + 300 * DAY);
    QCOMPARE(tracker.score("thumbsup").value(), 100.0);
    QCOMPARE(tracker.frecency("thumbsup").value(), 200);

    // Tracking dirties the entry, so the stale age is now folded in.
    tracker.track("thumbsup", NOW + 300 * DAY);
    tracker.compute(NOW + 300 * DAY);
    // weight(301) + weight(0) = 1 + 100
    QCOMPARE(tracker.score("thumbsup").value(), 101.0);
    // ceil(3 * (101 / 2)) = 152
    QCOMPARE(tracker.frecency("thumbsup").value(), 152);
}

void TestFrecency::testReactionFormula()
{
    FrecencyTracker tracker(FrecencyTracker::reactionEmojiOptions());

    QHash<QString, FrecencyTracker::Entry> history;
    history.insert("hot", makeEntry(3, { NOW - 40 * DAY, NOW - 10 * DAY, NOW - DAY }));
    history.insert("stale", makeEntry(7, { NOW - 100 * DAY }));
    tracker.overwriteHistory(history);
    tracker.compute(NOW);

    // maxTotalUse = 7 (from "stale"), scores are 200 and 1.
    QCOMPARE(tracker.score("hot").value(), 200.0);
    QCOMPARE(tracker.score("stale").value(), 1.0);
    // trunc(1000 * ((3/7)*0.2 + (200/1000)*0.8)) = trunc(245.71)
    QCOMPARE(tracker.frecency("hot").value(), 245);
    // trunc(1000 * ((7/7)*0.2 + (1/1000)*0.8)) = trunc(200.8)
    QCOMPARE(tracker.frecency("stale").value(), 200);
    QCOMPARE(tracker.frequently(NOW), QStringList({ "hot", "stale" }));
}

void TestFrecency::testCapForPersistence()
{
    QHash<QString, FrecencyTracker::Entry> history;
    FrecencyTracker::Entry newest = makeEntry(1, { NOW - 50 * DAY, NOW - DAY });
    newest.score = 12.4;
    FrecencyTracker::Entry middle = makeEntry(2, { NOW - 5 * DAY });
    middle.score = 7.6;
    FrecencyTracker::Entry oldest = makeEntry(9, { NOW - 90 * DAY });
    oldest.score = 1.0;
    history.insert("newest", newest);
    history.insert("middle", middle);
    history.insert("oldest", oldest);

    const auto capped = FrecencyTracker::capForPersistence(history, 2);
    QCOMPARE(capped.size(), 2);
    QVERIFY(capped.contains("newest"));
    QVERIFY(capped.contains("middle"));
    QVERIFY(!capped.contains("oldest"));

    QCOMPARE(capped.value("newest").score, 12.0);
    QCOMPARE(capped.value("middle").score, 8.0);
    QCOMPARE(capped.value("newest").totalUses, 1u);

    // Under the limit nothing is dropped.
    QCOMPARE(FrecencyTracker::capForPersistence(history, 100).size(), 3);
}

void TestFrecency::testOverwriteHistory()
{
    FrecencyTracker tracker;

    QHash<QString, FrecencyTracker::Entry> history;
    FrecencyTracker::Entry loaded = makeEntry(2, { NOW - DAY, 0, NOW - 3 * DAY });
    loaded.frecency = 999; // whatever the server had stored
    loaded.score = 55.0;
    history.insert("thumbsup", loaded);
    tracker.overwriteHistory(history);

    QVERIFY(tracker.isDirty());
    QCOMPARE(tracker.frecency("thumbsup").value(), -1);
    // Server timestamps arrive unsorted and may contain zeros.
    QCOMPARE(tracker.usageHistory().value("thumbsup").recentUses,
             QList<uint64_t>({ NOW - 3 * DAY, NOW - DAY }));

    tracker.frequently(NOW);
    // weight(3) + weight(1) = 200; ceil(2 * (200 / 2)) = 200
    QCOMPARE(tracker.score("thumbsup").value(), 200.0);
    QCOMPARE(tracker.frecency("thumbsup").value(), 200);

    // A second overwrite replaces the map wholesale.
    QHash<QString, FrecencyTracker::Entry> replacement;
    replacement.insert("eyes", makeEntry(1, { NOW - DAY }));
    tracker.overwriteHistory(replacement);
    QCOMPARE(tracker.frequently(NOW), QStringList({ "eyes" }));

    tracker.reset();
    QVERIFY(tracker.usageHistory().isEmpty());
    QVERIFY(tracker.frequently(NOW).isEmpty());
}

QTEST_GUILESS_MAIN(TestFrecency)
#include "tst_Frecency.moc"
