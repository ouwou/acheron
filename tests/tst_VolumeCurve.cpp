#include "Core/Audio/VolumeCurve.hpp"

#include <QTest>

using namespace Acheron::Core::Audio;

class TestVolumeCurve : public QObject
{
    Q_OBJECT
private slots:
    void matchesTheOfficialCurve()
    {
        QCOMPARE(VolumeCurve::gainAt(0), 0.0f);
        QCOMPARE(VolumeCurve::gainAt(VolumeCurve::UNCHANGED_POSITION), 1.0f);
        QVERIFY(qAbs(VolumeCurve::gainAt(50) - 0.14359f) < 1e-4f);
        QVERIFY(qAbs(VolumeCurve::gainAt(150) - 1.41254f) < 1e-4f);
        QVERIFY(qAbs(VolumeCurve::gainAt(VolumeCurve::MAX_POSITION) - 1.99526f) < 1e-4f);
    }

    void everyPositionSurvivesItsGain()
    {
        for (int position = 0; position <= VolumeCurve::MAX_POSITION; ++position)
            QCOMPARE(VolumeCurve::positionOf(VolumeCurve::gainAt(position)), position);
    }

    void linearGainsFromOlderBuildsLandOnTheCurve()
    {
        QCOMPARE(VolumeCurve::positionOf(0.5f), 78);
        QCOMPARE(VolumeCurve::positionOf(1.5f), 159);
        QCOMPARE(VolumeCurve::positionOf(2.0f), VolumeCurve::MAX_POSITION);
    }
};

QTEST_GUILESS_MAIN(TestVolumeCurve)
#include "tst_VolumeCurve.moc"
