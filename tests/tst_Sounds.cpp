#include "Core/Audio/Sounds.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>

using namespace Acheron::Core::Audio;

namespace {

constexpr int NO_SOUND = -1;

QByteArray mono16BitWav(int sampleRate, double seconds)
{
    const int frames = static_cast<int>(sampleRate * seconds);
    QByteArray pcm(frames * 2, Qt::Uninitialized);
    auto *samples = reinterpret_cast<int16_t *>(pcm.data());
    for (int i = 0; i < frames; i++)
        samples[i] = static_cast<int16_t>(std::lround(8000 * std::sin(2 * std::numbers::pi * 440 * i / sampleRate)));

    QByteArray wav;
    auto put32 = [&wav](uint32_t value) {
        for (int i = 0; i < 4; i++)
            wav.append(static_cast<char>((value >> (8 * i)) & 0xFF));
    };
    auto put16 = [&wav](uint16_t value) {
        wav.append(static_cast<char>(value & 0xFF));
        wav.append(static_cast<char>(value >> 8));
    };
    wav.append("RIFF");
    put32(36 + static_cast<uint32_t>(pcm.size()));
    wav.append("WAVEfmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(static_cast<uint32_t>(sampleRate));
    put32(static_cast<uint32_t>(sampleRate * 2));
    put16(2);
    put16(16);
    wav.append("data");
    put32(static_cast<uint32_t>(pcm.size()));
    wav.append(pcm);
    return wav;
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return {};
    return path;
}

double secondsOf(const std::shared_ptr<const SoundClip> &clip)
{
    return clip ? static_cast<double>(clip->frameCount()) / AUDIO_SAMPLE_RATE : 0.0;
}

} // namespace

class TestSounds : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanup();
    void testJoinLeaveRule_data();
    void testJoinLeaveRule();
    void testDecodesToPlaybackFormat();
    void testRejectsWhatIsNotASound();
    void testBuiltInSoundsLoad();
    void testCustomSoundReplacesBuiltIn();
    void testRejectedFileChangesNothing();
};

void TestSounds::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY2(customSoundsFolder().contains(QLatin1String("qttest")), qPrintable(customSoundsFolder()));
    cleanup();
}

void TestSounds::cleanup()
{
    QDir(customSoundsFolder()).removeRecursively();
}

void TestSounds::testJoinLeaveRule_data()
{
    QTest::addColumn<int>("before");
    QTest::addColumn<int>("after");
    QTest::addColumn<int>("expected");

    const int join = static_cast<int>(Sound::UserJoin);
    const int leave = static_cast<int>(Sound::UserLeave);
    QTest::newRow("someone joins an empty channel") << 0 << 1 << join;
    QTest::newRow("someone leaves") << 2 << 1 << leave;
    QTest::newRow("the last one leaves") << 1 << 0 << leave;
    QTest::newRow("nobody came or went") << 3 << 3 << NO_SOUND;
    QTest::newRow("several leave at once, one sound") << 5 << 0 << leave;
    QTest::newRow("25 others, one more joins") << 25 << 26 << join;
    QTest::newRow("26 others, one more joins") << 26 << 27 << NO_SOUND;
    QTest::newRow("26 others, one leaves") << 26 << 25 << NO_SOUND;
    QTest::newRow("25 others, one leaves") << 25 << 24 << leave;
}

void TestSounds::testJoinLeaveRule()
{
    QFETCH(int, before);
    QFETCH(int, after);
    QFETCH(int, expected);

    const std::optional<Sound> sound = joinLeaveSound(before, after);
    QCOMPARE(sound ? static_cast<int>(*sound) : NO_SOUND, expected);
}

void TestSounds::testDecodesToPlaybackFormat()
{
    const std::shared_ptr<const SoundClip> clip = decodeSound(mono16BitWav(44100, 0.5));
    QVERIFY(clip);
    QVERIFY2(std::abs(clip->frameCount() - AUDIO_SAMPLE_RATE / 2) < AUDIO_SAMPLE_RATE / 100, qPrintable(QString::number(clip->frameCount())));

    const auto *pcm = reinterpret_cast<const int16_t *>(clip->pcm.constData());
    int peak = 0;
    for (int i = 0; i < clip->frameCount(); i++) {
        QCOMPARE(pcm[i * AUDIO_CHANNELS], pcm[i * AUDIO_CHANNELS + 1]);
        peak = std::max(peak, std::abs(static_cast<int>(pcm[i * AUDIO_CHANNELS])));
    }
    QVERIFY2(peak > 7000 && peak < 9000, qPrintable(QString::number(peak)));
}

void TestSounds::testRejectsWhatIsNotASound()
{
    QVERIFY(!decodeSound(QByteArray()));
    QVERIFY(!decodeSound(QByteArray("this is not a sound, just some words")));
}

void TestSounds::testBuiltInSoundsLoad()
{
    for (Sound sound : ALL_SOUNDS) {
        QVERIFY(customSoundFile(sound).isEmpty());
        const std::shared_ptr<const SoundClip> clip = loadSound(sound);
        QVERIFY2(clip, qPrintable(soundName(sound)));
        QVERIFY2(secondsOf(clip) > 0.1 && secondsOf(clip) < 1.0, qPrintable(QString::number(secondsOf(clip))));
        QCOMPARE(loadSound(sound).get(), clip.get());
    }
}

void TestSounds::testCustomSoundReplacesBuiltIn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const double builtIn = secondsOf(loadSound(Sound::UserJoin));

    const QString chime = writeFile(dir, QStringLiteral("chime.wav"), mono16BitWav(48000, 0.5));
    QVERIFY(!setCustomSound(Sound::UserJoin, chime));
    QCOMPARE(QFileInfo(customSoundFile(Sound::UserJoin)).fileName(), QStringLiteral("user_join.wav"));
    QVERIFY(customSoundFile(Sound::UserLeave).isEmpty());
    QVERIFY(std::abs(secondsOf(loadSound(Sound::UserJoin)) - 0.5) < 0.01);

    const QString shorter = writeFile(dir, QStringLiteral("shorter.wav"), mono16BitWav(22050, 0.3));
    QVERIFY(!setCustomSound(Sound::UserJoin, shorter));
    QVERIFY(std::abs(secondsOf(loadSound(Sound::UserJoin)) - 0.3) < 0.01);

    resetCustomSound(Sound::UserJoin);
    QVERIFY(customSoundFile(Sound::UserJoin).isEmpty());
    QCOMPARE(secondsOf(loadSound(Sound::UserJoin)), builtIn);
}

void TestSounds::testRejectedFileChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString notes = writeFile(dir, QStringLiteral("notes.txt"), mono16BitWav(48000, 0.2));
    QVERIFY(setCustomSound(Sound::Disconnect, notes) == SoundFileProblem::NotWavMp3OrFlac);
    QVERIFY(customSoundFile(Sound::Disconnect).isEmpty());

    const QString good = writeFile(dir, QStringLiteral("good.wav"), mono16BitWav(48000, 0.2));
    QVERIFY(!setCustomSound(Sound::Disconnect, good));
    const QString kept = customSoundFile(Sound::Disconnect);

    const QString broken = writeFile(dir, QStringLiteral("broken.mp3"), QByteArray("not an mp3"));
    QVERIFY(setCustomSound(Sound::Disconnect, broken) == SoundFileProblem::NotASound);
    QCOMPARE(customSoundFile(Sound::Disconnect), kept);
}

QTEST_GUILESS_MAIN(TestSounds)
#include "tst_Sounds.moc"
