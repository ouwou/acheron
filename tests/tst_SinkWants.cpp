// the expected values are the official client's: its ladder (docs/video-re/re-voice-gateway-video.md,
// section 5) and its off-screen rules

#include "Core/Video/TileVisibility.hpp"
#include "Discord/Voice/SinkWants.hpp"

#include <QTest>

using namespace Acheron;
using namespace Acheron::Discord::Voice;
using Core::Snowflake;
using Core::TileVisibility;

namespace {

VideoSender camera(quint64 userId, quint32 highSsrc, bool twoLayers = true)
{
    VideoSender sender;
    sender.userId = Snowflake(userId);
    sender.layers.append(VideoLayer{ highSsrc, highSsrc + 1, 100 });
    if (twoLayers)
        sender.layers.append(VideoLayer{ highSsrc + 2, highSsrc + 3, 50 });
    return sender;
}

QByteArray json(const SinkWantsPlan &plan)
{
    return plan.wants.toJson().toBytes();
}

} // namespace

class TestSinkWants : public QObject
{
    Q_OBJECT
private slots:
    void ladderFollowsTheSenderCount_data();
    void ladderFollowsTheSenderCount();
    void ladderStepsDownForASmallTile();

    void nobodySending();
    void oneSenderWithOneEncoding();
    void oneSenderWithTwoEncodings();
    void severalSendersShareTheBudget();
    void focusedSenderGetsFullQuality();
    void focusChangesNothingWhenEveryoneIsAtFullQuality();
    void unwantedSenderGetsNothing();
    void callOfTwoSizesTheRequestToTheTile();

    void visibleTileIsWanted();
    void hiddenTileKeepsItsVideoForAWhile();
    void onlyTheMostRecentlyHiddenAreKept();
    void comingBackIntoViewResetsTheWait();
};

void TestSinkWants::ladderFollowsTheSenderCount_data()
{
    QTest::addColumn<int>("senders");
    QTest::addColumn<int>("level");
    QTest::newRow("1") << 1 << 100;
    QTest::newRow("2") << 2 << 90;
    QTest::newRow("3") << 3 << 80;
    QTest::newRow("4") << 4 << 70;
    QTest::newRow("5") << 5 << 60;
    QTest::newRow("6") << 6 << 60;
    QTest::newRow("7") << 7 << 50;
    QTest::newRow("9") << 9 << 50;
    QTest::newRow("10") << 10 << 40;
    QTest::newRow("11") << 11 << 40;
    QTest::newRow("12") << 12 << 30;
    QTest::newRow("16") << 16 << 30;
    QTest::newRow("17") << 17 << 20;
    QTest::newRow("25") << 25 << 20;
    QTest::newRow("40") << 40 << 20;
}

void TestSinkWants::ladderFollowsTheSenderCount()
{
    QFETCH(int, senders);
    QFETCH(int, level);
    QCOMPARE(sinkWantLevel(senders), level);
}

void TestSinkWants::ladderStepsDownForASmallTile()
{
    QCOMPARE(sinkWantLevel(1, 100000), 60);
    QCOMPARE(sinkWantLevel(1, 36863), 20);
    QCOMPARE(sinkWantLevel(1, 921600), 100);
    QCOMPARE(sinkWantLevel(1, 5000000), 100);
    QCOMPARE(sinkWantLevel(4, 900000), 70);
    QCOMPARE(sinkWantLevel(4, 100000), 60);
}

void TestSinkWants::nobodySending()
{
    const SinkWantsPlan plan = planSinkWants({});
    QCOMPARE(json(plan), QByteArray(R"({"any":100})"));
    QVERIFY(plan.deliver.isEmpty());
}

void TestSinkWants::oneSenderWithOneEncoding()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500, false) };

    const SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"any":100})"));
    QCOMPARE(plan.deliver.value(Snowflake(1)), 500u);
}

void TestSinkWants::oneSenderWithTwoEncodings()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };

    const SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"any":100})"));
    QCOMPARE(plan.deliver.value(Snowflake(1)), 502u);
}

void TestSinkWants::severalSendersShareTheBudget()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600), camera(3, 700) };

    const SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"600":0,"700":0,"any":80})"));
    QCOMPARE(plan.deliver.value(Snowflake(1)), 502u);
    QCOMPARE(plan.deliver.value(Snowflake(2)), 602u);
    QCOMPARE(plan.deliver.value(Snowflake(3)), 702u);
}

void TestSinkWants::focusedSenderGetsFullQuality()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600), camera(3, 700, false) };
    input.focusedUser = Snowflake(2);

    SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"600":100,"602":0,"any":80})"));
    QCOMPARE(plan.deliver.value(Snowflake(1)), 502u);
    QCOMPARE(plan.deliver.value(Snowflake(2)), 600u);
    QCOMPARE(plan.deliver.value(Snowflake(3)), 700u);

    input.focusedUser = Snowflake(3);
    plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"600":0,"700":100,"any":80})"));
    QCOMPARE(plan.deliver.value(Snowflake(3)), 700u);
}

void TestSinkWants::focusChangesNothingWhenEveryoneIsAtFullQuality()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };
    input.focusedUser = Snowflake(1);

    const SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"any":100})"));
    QCOMPARE(plan.deliver.value(Snowflake(1)), 502u);
}

void TestSinkWants::unwantedSenderGetsNothing()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500), camera(2, 600) };
    input.senders[1].wanted = false;

    const SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"600":0,"602":0,"any":90})"));
    QVERIFY(plan.deliver.contains(Snowflake(1)));
    QVERIFY(!plan.deliver.contains(Snowflake(2)));
}

void TestSinkWants::callOfTwoSizesTheRequestToTheTile()
{
    SinkWantsInput input;
    input.senders = { camera(1, 500) };
    input.senders[0].tilePixels = 100000;
    input.oneToOne = true;

    SinkWantsPlan plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":0,"502":60,"any":100})"));

    input.senders = { camera(1, 500, false) };
    input.senders[0].tilePixels = 100000;
    plan = planSinkWants(input);
    QCOMPARE(json(plan), QByteArray(R"({"500":60,"any":100})"));
}

void TestSinkWants::visibleTileIsWanted()
{
    TileVisibility tiles;
    QVERIFY(!tiles.wanted(Snowflake(1), 0));

    tiles.setVisible(Snowflake(1), true, 0);
    QVERIFY(tiles.wanted(Snowflake(1), 0));
    QVERIFY(tiles.wanted(Snowflake(1), 10 * TileVisibility::HIDDEN_GRACE_MS));
    QVERIFY(!tiles.hasPendingExpiry(0));
}

void TestSinkWants::hiddenTileKeepsItsVideoForAWhile()
{
    TileVisibility tiles;
    tiles.setVisible(Snowflake(1), true, 0);
    tiles.setVisible(Snowflake(1), false, 1000);

    QVERIFY(tiles.wanted(Snowflake(1), 1000));
    QVERIFY(tiles.wanted(Snowflake(1), 1000 + TileVisibility::HIDDEN_GRACE_MS - 1));
    QVERIFY(tiles.hasPendingExpiry(1000 + TileVisibility::HIDDEN_GRACE_MS - 1));
    QVERIFY(!tiles.wanted(Snowflake(1), 1000 + TileVisibility::HIDDEN_GRACE_MS));
    QVERIFY(!tiles.hasPendingExpiry(1000 + TileVisibility::HIDDEN_GRACE_MS));
}

void TestSinkWants::onlyTheMostRecentlyHiddenAreKept()
{
    TileVisibility tiles;
    for (quint64 user = 1; user <= 5; user++) {
        tiles.setVisible(Snowflake(user), true, 0);
        tiles.setVisible(Snowflake(user), false, static_cast<qint64>(user) * 100);
    }

    QVERIFY(!tiles.wanted(Snowflake(1), 600));
    QVERIFY(!tiles.wanted(Snowflake(2), 600));
    QVERIFY(tiles.wanted(Snowflake(3), 600));
    QVERIFY(tiles.wanted(Snowflake(4), 600));
    QVERIFY(tiles.wanted(Snowflake(5), 600));

    tiles.setVisible(Snowflake(5), true, 700);
    QVERIFY(tiles.wanted(Snowflake(2), 700));
    QVERIFY(!tiles.wanted(Snowflake(1), 700));
}

void TestSinkWants::comingBackIntoViewResetsTheWait()
{
    TileVisibility tiles;
    tiles.setVisible(Snowflake(1), true, 0);
    tiles.setVisible(Snowflake(1), false, 1000);
    tiles.setVisible(Snowflake(1), true, 20000);
    tiles.setVisible(Snowflake(1), false, 25000);

    QVERIFY(tiles.wanted(Snowflake(1), 1000 + TileVisibility::HIDDEN_GRACE_MS));
    QVERIFY(!tiles.wanted(Snowflake(1), 25000 + TileVisibility::HIDDEN_GRACE_MS));

    tiles.setVisible(Snowflake(1), false, 40000);
    QVERIFY(!tiles.wanted(Snowflake(1), 25000 + TileVisibility::HIDDEN_GRACE_MS));
}

QTEST_GUILESS_MAIN(TestSinkWants)
#include "tst_SinkWants.moc"
