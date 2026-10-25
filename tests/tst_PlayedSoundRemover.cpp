#include "Core/Audio/PlayedSoundRemover.hpp"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <vector>

using namespace Acheron::Core::Audio;

namespace {

constexpr int RATE = 48000;
constexpr int BLOCK = 480;
constexpr qint64 START = 1'000'000'000;
constexpr int ON_FRAMES = RATE / 5;
constexpr int OFF_FRAMES = RATE / 10;

qint64 timeOf(qint64 frame)
{
    return START + frame * 10'000'000 / RATE;
}

// noise in bursts, as speech comes
std::vector<int16_t> speechLike(int frames, float level, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<float> normal(0.0f, level * 32768.0f);
    std::vector<int16_t> stereo(static_cast<size_t>(frames) * 2, 0);
    for (int i = 0; i < frames; i++) {
        if (i % (ON_FRAMES + OFF_FRAMES) >= ON_FRAMES)
            continue;
        for (int channel = 0; channel < 2; channel++)
            stereo[static_cast<size_t>(i) * 2 + channel] = static_cast<int16_t>(std::clamp(normal(random), -32768.0f, 32767.0f));
    }
    return stereo;
}

std::vector<float> steadyNoise(int frames, float level, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<float> normal(0.0f, level);
    std::vector<float> stereo(static_cast<size_t>(frames) * 2);
    for (float &sample : stereo)
        sample = normal(random);
    return stereo;
}

struct Device
{
    float gain = 1.0f;
    int delay = 3000;
    bool echoes = true;
};

struct Run
{
    std::vector<float> output;
    std::vector<float> other;
};

Run play(PlayedSoundRemover &remover, PlayedSound &sound, const std::vector<int16_t> &played, const std::vector<float> &other, const std::function<Device(qint64)> &deviceAt)
{
    const int frames = static_cast<int>(other.size() / 2);
    Run run{ std::vector<float>(other.size()), other };
    for (int first = 0; first + BLOCK <= frames; first += BLOCK) {
        sound.record(played.data() + static_cast<size_t>(first) * 2, BLOCK, timeOf(first));

        std::vector<float> block(other.begin() + first * 2, other.begin() + (first + BLOCK) * 2);
        for (int i = 0; i < BLOCK; i++) {
            const Device device = deviceAt(first + i);
            const int heard = first + i - device.delay;
            if (!device.echoes || heard < 0)
                continue;
            for (int channel = 0; channel < 2; channel++)
                block[static_cast<size_t>(i) * 2 + channel] += device.gain * played[static_cast<size_t>(heard) * 2 + channel] / 32768.0f;
        }
        remover.remove(block.data(), BLOCK, timeOf(first));
        std::copy(block.begin(), block.end(), run.output.begin() + first * 2);
    }
    return run;
}

// how much of the output is not the other sound, over the last `frames`
double leftOver(const Run &run, int frames)
{
    double error = 0.0;
    double wanted = 0.0;
    for (size_t i = run.output.size() - static_cast<size_t>(frames) * 2; i < run.output.size(); i++) {
        error += (run.output[i] - run.other[i]) * (run.output[i] - run.other[i]);
        wanted += run.other[i] * run.other[i];
    }
    return error / wanted;
}

} // namespace

class TestPlayedSoundRemover : public QObject
{
    Q_OBJECT
private slots:
    void anEchoIsTakenOut_data();
    void anEchoIsTakenOut();
    void soundThatIsNotOursIsLeftAlone();
    void aNewDelayIsFound();
};

void TestPlayedSoundRemover::anEchoIsTakenOut_data()
{
    QTest::addColumn<float>("gain");
    QTest::addColumn<int>("delay");
    QTest::addColumn<float>("otherLevel");
    QTest::addColumn<float>("playedLevel");

    QTest::newRow("as played") << 1.0f << 3000 << 0.05f << 0.2f;
    QTest::newRow("turned down") << 0.4f << 3000 << 0.05f << 0.2f;
    QTest::newRow("long delay") << 1.0f << 20000 << 0.05f << 0.2f;
    QTest::newRow("under louder sound") << 1.0f << 1500 << 0.3f << 0.05f;
}

void TestPlayedSoundRemover::anEchoIsTakenOut()
{
    QFETCH(float, gain);
    QFETCH(int, delay);
    QFETCH(float, otherLevel);
    QFETCH(float, playedLevel);

    constexpr int FRAMES = RATE * 4;
    auto sound = std::make_shared<PlayedSound>();
    PlayedSoundRemover remover([sound] { return std::vector{ sound }; });
    const Run run = play(remover, *sound, speechLike(FRAMES, playedLevel, 1), steadyNoise(FRAMES, otherLevel, 2), [&](qint64) { return Device{ gain, delay }; });

    QCOMPARE(remover.delayOf(sound), std::optional<qint64>(delay));
    const double left = leftOver(run, RATE);
    qInfo() << "left over" << 10.0 * std::log10(left) << "dB";
    QVERIFY(left < 1e-3);
}

void TestPlayedSoundRemover::soundThatIsNotOursIsLeftAlone()
{
    constexpr int FRAMES = RATE * 4;
    auto sound = std::make_shared<PlayedSound>();
    PlayedSoundRemover remover([sound] { return std::vector{ sound }; });
    const Run run = play(remover, *sound, speechLike(FRAMES, 0.2f, 3), steadyNoise(FRAMES, 0.05f, 4), [](qint64) { return Device{ 1.0f, 3000, false }; });

    QVERIFY(!remover.delayOf(sound));
    QVERIFY(leftOver(run, FRAMES) < 1e-9);
}

void TestPlayedSoundRemover::aNewDelayIsFound()
{
    constexpr int FRAMES = RATE * 8;
    constexpr int SKIP_AT = RATE * 3;
    auto sound = std::make_shared<PlayedSound>();
    PlayedSoundRemover remover([sound] { return std::vector{ sound }; });
    const Run run = play(remover, *sound, speechLike(FRAMES, 0.2f, 5), steadyNoise(FRAMES, 0.05f, 6), [](qint64 frame) { return Device{ 1.0f, frame < SKIP_AT ? 3000 : 3480 }; });

    QCOMPARE(remover.delayOf(sound), std::optional<qint64>(3480));
    QVERIFY(leftOver(run, RATE) < 1e-3);
}

QTEST_GUILESS_MAIN(TestPlayedSoundRemover)
#include "tst_PlayedSoundRemover.moc"
