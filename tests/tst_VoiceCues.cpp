// Tests for Core::Audio::voiceCueFrames, the chimes played when someone joins or leaves the call:
// they must be whole frames in the pipeline's playback format, audible but well clear of clipping,
// start and end silent (no clicks), and rise for a join and fall for a leave.

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/VoiceCues.hpp"

#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

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

// a WAV file: format 1 (PCM) or 3 (float), samples already encoded
QString writeWav(const QTemporaryDir &dir, const QString &name, int format, int channels, int rate, int bits, const QByteArray &samples)
{
    auto le16 = [](QByteArray &b, quint16 v) { char c[2]; qToLittleEndian(v, c); b.append(c, 2); };
    auto le32 = [](QByteArray &b, quint32 v) { char c[4]; qToLittleEndian(v, c); b.append(c, 4); };
    QByteArray wav("RIFF");
    le32(wav, 4 + 8 + 16 + 8 + quint32(samples.size()));
    wav.append("WAVE");
    // an unrelated chunk first, as many editors write
    wav.append("LIST");
    le32(wav, 3);
    wav.append("abc", 3);
    wav.append('\0'); // pad to even
    wav.append("fmt ");
    le32(wav, 16);
    le16(wav, quint16(format));
    le16(wav, quint16(channels));
    le32(wav, quint32(rate));
    le32(wav, quint32(rate * channels * bits / 8));
    le16(wav, quint16(channels * bits / 8));
    le16(wav, quint16(bits));
    wav.append("data");
    le32(wav, quint32(samples.size()));
    wav.append(samples);

    const QString path = dir.filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(wav);
    return path;
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
    void testLoadMono16();
    void testLoadStereo24AndFloat();
    void testLoadRejects();
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
    const QVector<QByteArray> &frames = builtinVoiceCueFrames(cue);

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
    const std::vector<int16_t> samples = leftChannel(builtinVoiceCueFrames(cue));

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
    const std::vector<int16_t> samples = leftChannel(builtinVoiceCueFrames(cue));

    QCOMPARE(samples.front(), int16_t(0));
    QCOMPARE(samples.back(), int16_t(0));
    // the attack ramps up instead of jumping
    QVERIFY(std::abs(samples[1]) < INT16_MAX / 100);
}

void TestVoiceCues::testStereoChannelsMatch()
{
    for (VoiceCue cue : { VoiceCue::Join, VoiceCue::Leave, VoiceCue::Message }) {
        for (const QByteArray &frame : builtinVoiceCueFrames(cue)) {
            const auto *pcm = reinterpret_cast<const int16_t *>(frame.constData());
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++)
                QCOMPARE(pcm[i * AUDIO_CHANNELS], pcm[i * AUDIO_CHANNELS + 1]);
        }
    }
}

void TestVoiceCues::testJoinRisesLeaveFalls()
{
    // the first note alone, then the second once the first has rung out
    const std::vector<int16_t> join = leftChannel(builtinVoiceCueFrames(VoiceCue::Join));
    const std::vector<int16_t> leave = leftChannel(builtinVoiceCueFrames(VoiceCue::Leave));

    const double joinFirst = pitchBetween(join, 0.005, 0.075);
    const double joinSecond = pitchBetween(join, 0.15, 0.22);
    const double leaveFirst = pitchBetween(leave, 0.005, 0.075);
    const double leaveSecond = pitchBetween(leave, 0.15, 0.22);

    QVERIFY2(joinSecond > joinFirst * 1.2, qPrintable(QStringLiteral("%1 -> %2").arg(joinFirst).arg(joinSecond)));
    QVERIFY2(leaveSecond < leaveFirst / 1.2, qPrintable(QStringLiteral("%1 -> %2").arg(leaveFirst).arg(leaveSecond)));
}

void TestVoiceCues::testSameFramesEachTime()
{
    QCOMPARE(builtinVoiceCueFrames(VoiceCue::Join), builtinVoiceCueFrames(VoiceCue::Join));
    QVERIFY(builtinVoiceCueFrames(VoiceCue::Join) != builtinVoiceCueFrames(VoiceCue::Leave));
    QVERIFY(builtinVoiceCueFrames(VoiceCue::Message) != builtinVoiceCueFrames(VoiceCue::Join));
}

void TestVoiceCues::testMessageIsHigher()
{
    // told apart from a join by pitch: its first note is above the join's last
    const std::vector<int16_t> join = leftChannel(builtinVoiceCueFrames(VoiceCue::Join));
    const std::vector<int16_t> message = leftChannel(builtinVoiceCueFrames(VoiceCue::Message));
    QVERIFY(pitchBetween(message, 0.005, 0.04) > pitchBetween(join, 0.15, 0.22));
}

void TestVoiceCues::testMessageIsPlucked()
{
    // and by shape: three short plucks, so it goes nearly quiet between notes where a join keeps ringing
    const std::vector<int16_t> message = leftChannel(builtinVoiceCueFrames(VoiceCue::Message));
    const std::vector<int16_t> join = leftChannel(builtinVoiceCueFrames(VoiceCue::Join));
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
    QVERIFY(builtinVoiceCueFrames(VoiceCue::Message).size() < builtinVoiceCueFrames(VoiceCue::Join).size());
}

void TestVoiceCues::testLoadMono16()
{
    // half a second of a constant level at 44.1 kHz, mono: both channels, 48 kHz, whole frames
    QTemporaryDir dir;
    QByteArray samples;
    for (int i = 0; i < 22050; i++) {
        char c[2];
        qToLittleEndian<qint16>(16384, c);
        samples.append(c, 2);
    }
    QString error;
    const auto frames = loadCueSound(writeWav(dir, "mono.wav", 1, 1, 44100, 16, samples), &error);
    QVERIFY2(frames, qPrintable(error));
    QCOMPARE(frames->size(), 25); // 24000 samples at 48 kHz
    for (const QByteArray &frame : *frames)
        QCOMPARE(static_cast<int>(frame.size()), AUDIO_FRAME_SIZE);
    const auto *pcm = reinterpret_cast<const int16_t *>(frames->first().constData());
    QVERIFY(std::abs(pcm[100] - INT16_MAX / 2) <= 2);
    QCOMPARE(pcm[100], pcm[101]); // right matches left
}

void TestVoiceCues::testLoadStereo24AndFloat()
{
    QTemporaryDir dir;
    QString error;

    // 24-bit stereo: left a quarter up, right a quarter down
    QByteArray s24;
    for (int i = 0; i < 4800; i++) {
        const qint32 left = 0x200000, right = -0x200000;
        for (qint32 v : { left, right })
            s24.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)).append(char((v >> 16) & 0xFF));
    }
    const auto frames24 = loadCueSound(writeWav(dir, "s24.wav", 1, 2, 48000, 24, s24), &error);
    QVERIFY2(frames24, qPrintable(error));
    QCOMPARE(frames24->size(), 5);
    const auto *pcm = reinterpret_cast<const int16_t *>(frames24->first().constData());
    QVERIFY(std::abs(pcm[0] - INT16_MAX / 4) <= 2);
    QVERIFY(std::abs(pcm[1] + INT16_MAX / 4) <= 2);

    // 32-bit float, clipped past full scale
    QByteArray sf;
    for (int i = 0; i < 960; i++) {
        char c[4];
        qToLittleEndian<float>(i % 2 ? 2.0f : -0.5f, c);
        sf.append(c, 4);
    }
    const auto framesFloat = loadCueSound(writeWav(dir, "f32.wav", 3, 1, 48000, 32, sf), &error);
    QVERIFY2(framesFloat, qPrintable(error));
    const auto *fpcm = reinterpret_cast<const int16_t *>(framesFloat->first().constData());
    QCOMPARE(fpcm[2], INT16_MAX);
    QVERIFY(std::abs(fpcm[0] + INT16_MAX / 2) <= 2);
}

void TestVoiceCues::testLoadRejects()
{
    QTemporaryDir dir;
    QString error;

    QFile text(dir.filePath("notes.wav"));
    text.open(QIODevice::WriteOnly);
    text.write("this is not audio");
    text.close();
    QVERIFY(!loadCueSound(text.fileName(), &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!loadCueSound(dir.filePath("missing.wav"), &error));
    QVERIFY(!error.isEmpty());

    // a format it can't read (A-law)
    error.clear();
    QVERIFY(!loadCueSound(writeWav(dir, "alaw.wav", 6, 1, 8000, 8, QByteArray(800, 'x')), &error));
    QVERIFY(!error.isEmpty());

    // longer than allowed
    error.clear();
    const QByteArray tooLong((MAX_CUE_SECONDS + 1) * 8000, '\x80');
    QVERIFY(!loadCueSound(writeWav(dir, "long.wav", 1, 1, 8000, 8, tooLong), &error));
    QVERIFY(error.contains(QString::number(MAX_CUE_SECONDS)));
}

QTEST_GUILESS_MAIN(TestVoiceCues)
#include "tst_VoiceCues.moc"
