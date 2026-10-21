#include "Core/Media/DecodeDemand.hpp"

#include <QTest>

using Acheron::Core::Media::DecodeDemand;

namespace {

constexpr int KEEPING_UP = 0;
constexpr int BEHIND = DecodeDemand::BEHIND_MS;
constexpr int FULL_HD = 1920 * 1080;

bool observeFor(DecodeDemand &demand, qint64 &nowMs, int seconds, int backlogMs, quint64 discards = 0)
{
    bool changed = false;
    for (int second = 0; second < seconds; second++) {
        nowMs += 1000;
        changed = demand.observe(backlogMs, discards, nowMs) || changed;
    }
    return changed;
}

} // namespace

class TestDecodeDemand : public QObject
{
    Q_OBJECT
private slots:
    void aShortBacklogChangesNothing();
    void stayingBehindHalvesWhatIsAskedFor();
    void theSenderIsGivenTimeBeforeTheNextHalving();
    void aDiscardedQueueHalvesAtOnce();
    void keepingUpRaisesItAgain();
    void aRaiseTakenBackMakesTheNextOneWaitLonger();
    void halfOfWhatIsSentNotOfTheSurface();
};

void TestDecodeDemand::aShortBacklogChangesNothing()
{
    DecodeDemand demand;
    qint64 now = 0;
    QVERIFY(!observeFor(demand, now, 30, KEEPING_UP));
    QVERIFY(!observeFor(demand, now, 1, BEHIND));
    QVERIFY(!observeFor(demand, now, 30, KEEPING_UP));
    QCOMPARE(demand.pixelShare(), 1.0);
}

void TestDecodeDemand::stayingBehindHalvesWhatIsAskedFor()
{
    DecodeDemand demand;
    qint64 now = 0;
    QVERIFY(!observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000, BEHIND));
    QVERIFY(observeFor(demand, now, 1, BEHIND));
    QCOMPARE(demand.pixelShare(), 0.5);
}

void TestDecodeDemand::theSenderIsGivenTimeBeforeTheNextHalving()
{
    DecodeDemand demand;
    qint64 now = 0;
    QVERIFY(observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND));
    QVERIFY(!observeFor(demand, now, DecodeDemand::SETTLE_MS / 1000 - 1, BEHIND));
    QVERIFY(observeFor(demand, now, 1, BEHIND));
    QCOMPARE(demand.pixelShare(), 0.25);

    QVERIFY(!observeFor(demand, now, 60, BEHIND));
    QCOMPARE(demand.pixelShare(), 0.25);
}

void TestDecodeDemand::aDiscardedQueueHalvesAtOnce()
{
    DecodeDemand demand;
    qint64 now = 0;
    QVERIFY(!observeFor(demand, now, 5, KEEPING_UP));
    QVERIFY(observeFor(demand, now, 1, KEEPING_UP, 1));
    QCOMPARE(demand.pixelShare(), 0.5);
    QVERIFY(!observeFor(demand, now, 10, KEEPING_UP, 1));
}

void TestDecodeDemand::keepingUpRaisesItAgain()
{
    DecodeDemand demand;
    qint64 now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    QCOMPARE(demand.pixelShare(), 0.5);

    QVERIFY(!observeFor(demand, now, DecodeDemand::FIRST_RAISE_AFTER_MS / 1000, KEEPING_UP));
    QVERIFY(observeFor(demand, now, 1, KEEPING_UP));
    QCOMPARE(demand.pixelShare(), 1.0);
}

void TestDecodeDemand::aRaiseTakenBackMakesTheNextOneWaitLonger()
{
    DecodeDemand demand;
    qint64 now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    observeFor(demand, now, DecodeDemand::FIRST_RAISE_AFTER_MS / 1000 + 1, KEEPING_UP);
    QCOMPARE(demand.pixelShare(), 1.0);

    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    QCOMPARE(demand.pixelShare(), 0.5);

    QVERIFY(!observeFor(demand, now, 2 * DecodeDemand::FIRST_RAISE_AFTER_MS / 1000, KEEPING_UP));
    QVERIFY(observeFor(demand, now, 1, KEEPING_UP));
    QCOMPARE(demand.pixelShare(), 1.0);
}

void TestDecodeDemand::halfOfWhatIsSentNotOfTheSurface()
{
    DecodeDemand demand;
    constexpr int SURFACE = 3840 * 2160;
    QCOMPARE(demand.pixelsToAskFor(SURFACE, FULL_HD), SURFACE);

    qint64 now = 0;
    observeFor(demand, now, DecodeDemand::BEHIND_FOR_MS / 1000 + 1, BEHIND);
    QCOMPARE(demand.pixelsToAskFor(SURFACE, FULL_HD), FULL_HD / 2);
    QCOMPARE(demand.pixelsToAskFor(FULL_HD / 4, FULL_HD), FULL_HD / 8);
    QCOMPARE(demand.pixelsToAskFor(SURFACE, 0), SURFACE / 2);
}

QTEST_GUILESS_MAIN(TestDecodeDemand)
#include "tst_DecodeDemand.moc"
