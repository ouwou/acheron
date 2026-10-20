#include "UI/Call/CallTileModel.hpp"

#include <QTest>

using Acheron::Core::Snowflake;
using Acheron::UI::arrangeTiles;
using Acheron::UI::CallFacts;
using Acheron::UI::CallParticipant;
using Acheron::UI::MAX_STRIP_TILES;
using Acheron::UI::planTiles;
using Acheron::UI::TileKey;
using Acheron::UI::TilePlan;

namespace {

const Snowflake SELF(1);
const Snowflake ALICE(2);
const Snowflake BOB(3);

CallFacts callWith(const QList<CallParticipant> &others)
{
    CallFacts facts;
    facts.accountId = SELF;
    facts.participants.append({ SELF });
    facts.participants.append(others);
    return facts;
}

} // namespace

class TestCallTiles : public QObject
{
    Q_OBJECT
private slots:
    void aCallWithoutVideoShowsEveryoneElse()
    {
        const TilePlan plan = planTiles(callWith({ { ALICE }, { BOB } }));
        QCOMPARE(plan.videoCount, 0);
        QCOMPARE(plan.tiles, (QList<TileKey>{ TileKey::personOf(ALICE), TileKey::personOf(BOB) }));
    }

    void aCollapsedCallWithoutVideoHasNoTiles()
    {
        CallFacts facts = callWith({ { ALICE } });
        facts.collapsedToBar = true;
        QVERIFY(planTiles(facts).tiles.isEmpty());
    }

    void videoOpensACollapsedCall()
    {
        CallFacts facts = callWith({ { ALICE, true } });
        facts.collapsedToBar = true;
        const TilePlan plan = planTiles(facts);
        QCOMPARE(plan.videoCount, 1);
        QVERIFY(plan.tiles.contains(TileKey::streamOf(ALICE)));
    }

    void withVideoTheOthersFollowTheCheckbox()
    {
        CallFacts facts = callWith({ { ALICE, false, true }, { BOB } });
        QVERIFY(planTiles(facts).tiles.contains(TileKey::personOf(BOB)));

        facts.nonVideoShown = false;
        const TilePlan plan = planTiles(facts);
        QCOMPARE(plan.tiles, QList<TileKey>{ TileKey::personOf(ALICE) });
    }

    void aDisabledCameraHasNoTileAndNoVideo()
    {
        const TilePlan plan = planTiles(callWith({ { ALICE, false, true, true }, { BOB } }));
        QCOMPARE(plan.videoCount, 0);
        QVERIFY(!plan.tiles.contains(TileKey::personOf(ALICE)));
        QVERIFY(plan.tiles.contains(TileKey::personOf(BOB)));
    }

    void theAccountsOwnVideoComesFromItsSenders()
    {
        CallFacts facts = callWith({});
        facts.participants[0].streaming = true;
        QCOMPARE(planTiles(facts).videoCount, 0);

        facts.ownShareShown = true;
        facts.ownCameraShown = true;
        const TilePlan plan = planTiles(facts);
        QCOMPARE(plan.videoCount, 2);
        QCOMPARE(plan.tiles, (QList<TileKey>{ TileKey::streamOf(SELF), TileKey::personOf(SELF) }));
    }

    void tilesFillAGrid()
    {
        const QRect area(0, 0, 1000, 600);
        const QList<QRect> places = arrangeTiles(5, std::nullopt, area);
        QCOMPARE(places.size(), 5);
        for (const QRect &place : places)
            QVERIFY(area.contains(place));
        QCOMPARE(places[0].size(), places[4].size());
        QCOMPARE(places[3].top(), places[0].bottom() + 1 + Acheron::UI::TILE_GAP);
    }

    void aFocusedTileTakesMostOfTheArea()
    {
        const QRect area(0, 0, 1000, 600);
        const QList<QRect> places = arrangeTiles(3, 1, area);
        QCOMPARE(places[1].width(), area.width());
        QVERIFY(places[1].height() > area.height() / 2);
        QVERIFY(places[0].top() > places[1].bottom());
        QCOMPARE(places[0].height(), places[2].height());
    }

    void aFocusedTileAloneFillsTheArea()
    {
        const QRect area(0, 0, 640, 360);
        QCOMPARE(arrangeTiles(1, 0, area), QList<QRect>{ area });
    }

    void theStripHoldsOnlySoMany()
    {
        const QList<QRect> places = arrangeTiles(MAX_STRIP_TILES + 3, 0, QRect(0, 0, 1920, 1080));
        int hidden = 0;
        for (const QRect &place : places)
            hidden += place.isEmpty() ? 1 : 0;
        QCOMPARE(hidden, 2);
    }

    void noTilesHaveNoPlaces()
    {
        QVERIFY(arrangeTiles(0, std::nullopt, QRect(0, 0, 100, 100)).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCallTiles)
#include "tst_CallTiles.moc"
