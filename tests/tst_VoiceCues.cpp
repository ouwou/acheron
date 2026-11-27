// Tests for Core::Audio::voiceCueFrames, the chimes played when someone joins or leaves the call:
// they must be whole frames in the pipeline's playback format, audible but well clear of clipping,
// start and end silent (no clicks), and rise for a join and fall for a leave.

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/VoiceCues.hpp"

#include <QTest>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace Acheron::Core::Audio;

Q_DECLARE_METATYPE(VoiceCue)

namespace {

// left channel of every frame, one after another
std::vector<int16_t> leftChannel(const QVector<QByteArray> &frames)
{
    std::vector<int16_t> samples;
    for (const QByteArray &frame : frames) {
        const auto *pcm = reinterpret_cast<const int16_t *>(frame.constData());
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++)
            samples.push_back(pcm[i * AUDIO_CHANNELS]);
    }
    return samples;
}

// upward zero crossings per second over [from, to) seconds: the pitch of a near-pure tone
double pitchBetween(const std::vector<int16_t> &samples, double from, double to)
{
    const auto first = static_cast<size_t>(from * AUDIO_SAMPLE_RATE);
    const auto last = static_cast<size_t>(to * AUDIO_SAMPLE_RATE);
    int crossings = 0;
    for (size_t i = first + 1; i < last && i < samples.size(); i++) {
        if (samples[i - 1] < 0 && samples[i] >= 0)
            crossings++;
    }
    return crossings / (to - from);
}

} // namespace

class TestVoiceCues : public QObject
{
    Q_OBJECT
private slots:
    void testFrameFormat_data();
    void testFrameFormat();
    void testLevel_data();
    void testLevel();
    void testStartsAndEndsSilent_data();
    void testStartsAndEndsSilent();
    void testStereoChannelsMatch();
    void testJoinRisesLeaveFalls();
    void testSameFramesEachTime();
    void testMessageIsHigher();
    void testMessageIsPlucked();
};

void TestVoiceCues::testFrameFormat_data()
{
    QTest::addColumn<VoiceCue>("cue");
    QTest::newRow("join") << VoiceCue::Join;
    QTest::newRow("leave") << VoiceCue::Leave;
    QTest::newRow("message") << VoiceCue::Message;
}

void TestVoiceCues::testFrameFormat()
{
    QFETCH(VoiceCue, cue);
    const QVector<QByteArray> &frames = voiceCueFrames(cue);

    // short: a quarter of a second or so, not a jingle
    QVERIFY(frames.size() >= 5);
    QVERIFY(frames.size() <= 25);
    for (const QByteArray &frame : frames)
        QCOMPARE(static_cast<int>(frame.size()), AUDIO_FRAME_SIZE);
}

void TestVoiceCues::testLevel_data()
{
    testFrameFormat_data();
}

void TestVoiceCues::testLevel()
{
    QFETCH(VoiceCue, cue);
    const std::vector<int16_t> samples = leftChannel(voiceCueFrames(cue));

    int peak = 0;
    for (int16_t s : samples)
        peak = std::max(peak, std::abs(static_cast<int>(s)));

    // audible, but leaves room for the voices it's mixed with
    QVERIFY2(peak > INT16_MAX / 10, qPrintable(QString::number(peak)));
    QVERIFY2(peak < INT16_MAX / 2, qPrintable(QString::number(peak)));
}

void TestVoiceCues::testStartsAndEndsSilent_data()
{
    testFrameFormat_data();
}

void TestVoiceCues::testStartsAndEndsSilent()
{
    QFETCH(VoiceCue, cue);
    const std::vector<int16_t> samples = leftChannel(voiceCueFrames(cue));

    QCOMPARE(samples.front(), int16_t(0));
    QCOMPARE(samples.back(), int16_t(0));
    // the attack ramps up instead of jumping
    QVERIFY(std::abs(samples[1]) < INT16_MAX / 100);
}

void TestVoiceCues::testStereoChannelsMatch()
{
    for (VoiceCue cue : { VoiceCue::Join, VoiceCue::Leave, VoiceCue::Message }) {
        for (const QByteArray &frame : voiceCueFrames(cue)) {
            const auto *pcm = reinterpret_cast<const int16_t *>(frame.constData());
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++)
                QCOMPARE(pcm[i * AUDIO_CHANNELS], pcm[i * AUDIO_CHANNELS + 1]);
        }
    }
}

void TestVoiceCues::testJoinRisesLeaveFalls()
{
    // the first note alone, then the second once the first has rung out
    const std::vector<int16_t> join = leftChannel(voiceCueFrames(VoiceCue::Join));
    const std::vector<int16_t> leave = leftChannel(voiceCueFrames(VoiceCue::Leave));

    const double joinFirst = pitchBetween(join, 0.005, 0.075);
    const double joinSecond = pitchBetween(join, 0.15, 0.22);
    const double leaveFirst = pitchBetween(leave, 0.005, 0.075);
    const double leaveSecond = pitchBetween(leave, 0.15, 0.22);

    QVERIFY2(joinSecond > joinFirst * 1.2, qPrintable(QStringLiteral("%1 -> %2").arg(joinFirst).arg(joinSecond)));
    QVERIFY2(leaveSecond < leaveFirst / 1.2, qPrintable(QStringLiteral("%1 -> %2").arg(leaveFirst).arg(leaveSecond)));
}

void TestVoiceCues::testSameFramesEachTime()
{
    // generated once and shared
    QCOMPARE(&voiceCueFrames(VoiceCue::Join), &voiceCueFrames(VoiceCue::Join));
    QVERIFY(voiceCueFrames(VoiceCue::Join) != voiceCueFrames(VoiceCue::Leave));
    QVERIFY(voiceCueFrames(VoiceCue::Message) != voiceCueFrames(VoiceCue::Join));
}

void TestVoiceCues::testMessageIsHigher()
{
    // told apart from a join by pitch: its first note is above the join's last
    const std::vector<int16_t> join = leftChannel(voiceCueFrames(VoiceCue::Join));
    const std::vector<int16_t> message = leftChannel(voiceCueFrames(VoiceCue::Message));
    QVERIFY(pitchBetween(message, 0.005, 0.04) > pitchBetween(join, 0.15, 0.22));
}

void TestVoiceCues::testMessageIsPlucked()
{
    // and by shape: three short plucks, so it goes nearly quiet between notes where a join keeps ringing
    const std::vector<int16_t> message = leftChannel(voiceCueFrames(VoiceCue::Message));
    const std::vector<int16_t> join = leftChannel(voiceCueFrames(VoiceCue::Join));
    auto peakBetween = [](const std::vector<int16_t> &samples, double from, double to) {
        int peak = 0;
        for (auto i = static_cast<size_t>(from * AUDIO_SAMPLE_RATE); i < static_cast<size_t>(to * AUDIO_SAMPLE_RATE) && i < samples.size(); i++)
            peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        return peak;
    };
    // just before the second pluck, against its own start
    QVERIFY(peakBetween(message, 0.045, 0.054) * 4 < peakBetween(message, 0.0, 0.02));
    // a join at the same moment is still ringing
    QVERIFY(peakBetween(join, 0.045, 0.054) * 4 > peakBetween(join, 0.0, 0.02));
    // and it's over sooner
    QVERIFY(voiceCueFrames(VoiceCue::Message).size() < voiceCueFrames(VoiceCue::Join).size());
}

QTEST_GUILESS_MAIN(TestVoiceCues)
#include "tst_VoiceCues.moc"
