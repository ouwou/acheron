
#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/LoopbackCapture.hpp"
#include "Core/Streams/StreamAudio.hpp"
#include "HardwareTests.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
#define MINIAUDIO_IMPLEMENTATION
#include "Core/Audio/Miniaudio.hpp"
#endif

#ifdef Q_OS_WIN
#include <tlhelp32.h>
#endif

#include "ProcessUsage.hpp"

#ifdef ACHERON_HAVE_PIPEWIRE
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <mutex>

#include <signal.h>
#include <sys/resource.h>
#endif

using namespace Acheron::Core;
using Audio::LoopbackCapture;
using ProcessUsage::threadsOfThisProcess;

namespace {

constexpr double TONE_HZ = 1375;
constexpr float TONE_AMPLITUDE = 0.02f;
constexpr int TONE_MS = 400;
constexpr int TONE_FADE_MS = 15;
constexpr int CAPTURE_STARTS_MS = 250;
constexpr int SOUND_ARRIVES_MS = 200;

const char CANNOT_CAPTURE[] = "This machine cannot capture one program's sound.";
const char CANNOT_PLAY[] = "This machine has no sound output to play a tone on.";

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)

class Tone
{
public:
    Tone()
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = Audio::AUDIO_CHANNELS;
        config.sampleRate = Audio::AUDIO_SAMPLE_RATE;
        config.dataCallback = &Tone::fill;
        config.pUserData = this;
        // only WASAPI, where the capture listens. left to choose, miniaudio ends at a backend that plays to
        // nowhere on a machine with no sound card
        const ma_backend backends[] = { ma_backend_wasapi };
        hasContext = ma_context_init(backends, 1, nullptr, &context) == MA_SUCCESS;
        opened = hasContext && ma_device_init(&context, &config, &device) == MA_SUCCESS;
        playing = opened && ma_device_start(&device) == MA_SUCCESS;
    }

    ~Tone()
    {
        if (opened)
            ma_device_uninit(&device);
        if (hasContext)
            ma_context_uninit(&context);
    }

    [[nodiscard]] bool isPlaying() const { return playing; }

private:
    static void fill(ma_device *device, void *output, const void *, ma_uint32 sampleFrames)
    {
        constexpr int LENGTH = TONE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;
        constexpr float FADE = TONE_FADE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;

        auto *tone = static_cast<Tone *>(device->pUserData);
        auto *samples = static_cast<float *>(output);
        for (ma_uint32 i = 0; i < sampleFrames; i++) {
            float sample = 0;
            if (tone->position < LENGTH) {
                const float envelope = qMin(1.0f, qMin(tone->position, LENGTH - tone->position) / FADE);
                sample = TONE_AMPLITUDE * envelope * static_cast<float>(std::sin(2 * std::numbers::pi * TONE_HZ * tone->position / Audio::AUDIO_SAMPLE_RATE));
                tone->position++;
            }
            samples[2 * i] = sample;
            samples[2 * i + 1] = sample;
        }
    }

    ma_context context;
    bool hasContext = false;
    ma_device device;
    bool opened = false;
    bool playing = false;
    int position = 0;
};

#else

class Tone
{
public:
    [[nodiscard]] bool isPlaying() const { return false; }
};

#endif

#ifdef Q_OS_WIN

bool processExists(quint32 processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
        CloseHandle(process);
    return process != nullptr;
}

#elif defined(Q_OS_LINUX)

bool processExists(quint32 processId)
{
    return QFileInfo::exists(QStringLiteral("/proc/%1").arg(processId));
}

#else

bool processExists(quint32)
{
    return false;
}

#endif

struct Recording : QObject
{
    Recording()
    {
        connect(&capture, &LoopbackCapture::frameCaptured, this, [this](const QByteArray &pcm) { frames.append(pcm); });
        connect(&capture, &LoopbackCapture::failed, this, [this](const QString &reason) { failures.append(reason); });
    }

    [[nodiscard]] bool everyFrameIsWhole() const
    {
        return std::all_of(frames.cbegin(), frames.cend(), [](const QByteArray &pcm) { return pcm.size() == Audio::AUDIO_FRAME_SIZE; });
    }

    [[nodiscard]] QByteArray sound() const
    {
        QByteArray joined;
        for (const QByteArray &pcm : frames) {
            if (!isSilence(pcm))
                joined.append(pcm);
        }
        return joined;
    }

    LoopbackCapture capture;
    QList<QByteArray> frames;
    QStringList failures;
};

double level(const QByteArray &pcm, double hz)
{
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int sampleFrames = static_cast<int>(pcm.size()) / (Audio::AUDIO_CHANNELS * Audio::AUDIO_SAMPLE_BYTES);
    if (sampleFrames == 0)
        return 0;

    double real = 0;
    double imaginary = 0;
    for (int i = 0; i < sampleFrames; i++) {
        const double mono = (samples[2 * i] + samples[2 * i + 1]) / 2.0;
        const double angle = 2 * std::numbers::pi * hz * i / Audio::AUDIO_SAMPLE_RATE;
        real += mono * std::cos(angle);
        imaginary += mono * std::sin(angle);
    }
    return 2 * std::hypot(real, imaginary) / sampleFrames;
}

bool hasTone(const QByteArray &pcm)
{
    constexpr double BESIDE_HZ = 97;
    const double tone = level(pcm, TONE_HZ);
    const double beside = qMax(level(pcm, TONE_HZ - BESIDE_HZ), level(pcm, TONE_HZ + BESIDE_HZ));
    return tone > 0.3 * TONE_AMPLITUDE * 32767 && tone > 4 * beside;
}

#ifdef ACHERON_HAVE_PIPEWIRE

const char CANNOT_RUN[] = "This machine cannot run a PipeWire of the test's own.";
const char ASKING_ALONE[] = "ACHERON_TEST_ASKS_ALONE";

const char SOUNDLESS_PIPEWIRE[] = R"(
context.properties = { core.daemon = true core.name = pipewire-0 }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-access }
    { name = libpipewire-module-metadata }
]
context.objects = [
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
    { factory = adapter args = { factory.name = support.null-audio-sink node.name = test-sink media.class = Audio/Sink audio.position = [ SINK_CHANNELS ] } }
]
)";

// a private PipeWire with a session manager that manages no devices; while it lives this process
// captures from it
class OwnPipeWire
{
public:
    explicit OwnPipeWire(const char *sinkChannels = "FL FR")
    {
        QFile configuration(directory.filePath(QStringLiteral("pipewire.conf")));
        if (!configuration.open(QIODevice::WriteOnly))
            return;
        configuration.write(QByteArray(SOUNDLESS_PIPEWIRE).replace("SINK_CHANNELS", sinkChannels));
        configuration.close();

        for (const char *name : { "DBUS_SESSION_BUS_ADDRESS", "PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", ASKING_ALONE })
            environment.remove(QLatin1String(name));
        for (const char *name : { "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "XDG_CONFIG_HOME" })
            environment.insert(QLatin1String(name), directory.path());
        for (QProcess *process : { &daemon, &sessionManager })
            process->setProcessEnvironment(environment);
        qputenv("PIPEWIRE_RUNTIME_DIR", directory.path().toLocal8Bit());

        // should the test die first, `timeout` ends the daemon and everything connected to it
        daemon.start(QStringLiteral("timeout"), { QStringLiteral("60"), QStringLiteral("pipewire"), QStringLiteral("-c"), configuration.fileName() });
        if (!QTest::qWaitFor([this] { return QFileInfo::exists(directory.filePath(QStringLiteral("pipewire-0"))); }, 5000))
            return;
        sessionManager.start(QStringLiteral("wireplumber"), { QStringLiteral("-p"), QStringLiteral("policy") });
        running = QTest::qWaitFor([this] { return ask(QStringLiteral("pw-link"), { QStringLiteral("-i") }).contains("test-sink:playback_FL"); }, 10000);
    }

    ~OwnPipeWire()
    {
        if (runtimeBefore.isNull())
            qunsetenv("PIPEWIRE_RUNTIME_DIR");
        else
            qputenv("PIPEWIRE_RUNTIME_DIR", runtimeBefore);
        for (QProcess *process : { &sessionManager, &daemon }) {
            process->terminate();
            process->waitForFinished(2000);
        }
    }

    QByteArray ask(const QString &tool, const QStringList &arguments) const
    {
        QProcess asked;
        asked.setProcessEnvironment(environment);
        asked.start(tool, arguments);
        asked.waitForFinished(2000);
        return asked.readAllStandardOutput();
    }

    void play(QProcess &player, const QByteArray &pcm, const QString &channels, const QStringList &more = {}) const
    {
        player.setProcessEnvironment(environment);
        player.start(QStringLiteral("pw-cat"), QStringList{ QStringLiteral("--playback"), QStringLiteral("--raw"), QStringLiteral("--format"), QStringLiteral("s16"), QStringLiteral("--rate"), QString::number(Audio::AUDIO_SAMPLE_RATE), QStringLiteral("--channels"), QString::number(channels.count(QLatin1Char(',')) + 1), QStringLiteral("--channel-map"), channels } + more + QStringList{ QStringLiteral("-") });
        player.write(pcm);
        player.closeWriteChannel();
    }

    QTemporaryDir directory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    bool running = false;

private:
    QProcess daemon;
    QProcess sessionManager;
    const QByteArray runtimeBefore = qgetenv("PIPEWIRE_RUNTIME_DIR");
};

QByteArray toneAsPcm(int channels, int onlyOn = -1)
{
    constexpr int LENGTH = TONE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;
    constexpr float FADE = TONE_FADE_MS * Audio::AUDIO_SAMPLE_RATE / 1000;

    QByteArray pcm(LENGTH * channels * Audio::AUDIO_SAMPLE_BYTES, '\0');
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < LENGTH; i++) {
        const float envelope = qMin(1.0f, qMin(i, LENGTH - i) / FADE);
        const auto sample = static_cast<qint16>(32767 * TONE_AMPLITUDE * envelope * std::sin(2 * std::numbers::pi * TONE_HZ * i / Audio::AUDIO_SAMPLE_RATE));
        for (int channel = 0; channel < channels; channel++)
            samples[channels * i + channel] = onlyOn < 0 || onlyOn == channel ? sample : 0;
    }
    return pcm;
}

QByteArray oneSideOf(QByteArray pcm, int side)
{
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < pcm.size() / 4; i++)
        samples[2 * i] = samples[2 * i + 1] = samples[2 * i + side];
    return pcm;
}

#endif

QByteArray frameOf(qint16 sample)
{
    QByteArray pcm(Audio::AUDIO_FRAME_SIZE, '\0');
    std::fill_n(reinterpret_cast<qint16 *>(pcm.data()), pcm.size() / 2, sample);
    return pcm;
}

} // namespace

class TestLoopbackCapture : public QObject
{
    Q_OBJECT
private slots:
#ifdef ACHERON_HAVE_PIPEWIRE
    void anAnswerOfNoIsAskedAgain();
#endif
    void whetherItIsAvailableIsFoundOutOnce();
    void whatThisProgramPlaysIsCaptured();
    void everythingButThisProgramLeavesItsToneOut();
    void aProcessThatDoesNotExistHasNoSound();
    void startingAndStoppingLeavesNoThreadBehind();
    void thisProgramIsNotAmongThePlayingPrograms();
#ifdef ACHERON_HAVE_PIPEWIRE
    void whatALoopbackPlaysAgainIsLeftOut();
    void framesComeEvenlyHoweverLongTheGraphsCycle();
    void surroundSoundIsMixedDownToBothSides();
    void startingWithNoFileLeftToOpenFails();
#endif

    void silenceIsWhatRoundingLeaves();
    void aRunStartsWithSoundAndOutlastsAShortSilence();
    void soundDuringTheSilenceKeepsTheRunOpen();
    void aRunEndsWhenFramesStopComing();
};

void TestLoopbackCapture::whetherItIsAvailableIsFoundOutOnce()
{
    const bool available = Audio::processLoopbackAvailable();
#if !defined(Q_OS_WIN) && !defined(ACHERON_HAVE_PIPEWIRE)
    QVERIFY(!available);
#endif

    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 1000; i++)
        QCOMPARE(Audio::processLoopbackAvailable(), available);
    QVERIFY(timer.elapsed() < 100);
}

void TestLoopbackCapture::whatThisProgramPlaysIsCaptured()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    Recording own;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);
    own.capture.stop();
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    own.capture.stop();

    QVERIFY2(own.failures.isEmpty(), qPrintable(own.failures.join(' ')));
    QVERIFY(own.everyFrameIsWhole());
    const int framesOfTone = TONE_MS / Audio::AUDIO_FRAME_DURATION_MS;
    const int framesOfSound = static_cast<int>(own.sound().size()) / Audio::AUDIO_FRAME_SIZE;
    QVERIFY2(framesOfSound >= framesOfTone - 2 && framesOfSound <= framesOfTone + 2, qPrintable(QString::number(framesOfSound)));
    QVERIFY(hasTone(own.sound()));
}

void TestLoopbackCapture::everythingButThisProgramLeavesItsToneOut()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    Recording own;
    Recording others;
    own.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    others.capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    own.capture.stop();
    others.capture.stop();

    QVERIFY2(own.failures.isEmpty(), qPrintable(own.failures.join(' ')));
    QVERIFY2(others.failures.isEmpty(), qPrintable(others.failures.join(' ')));
    QVERIFY(hasTone(own.sound()));
    QVERIFY(others.everyFrameIsWhole());
    QVERIFY(!hasTone(others.sound()));
}

void TestLoopbackCapture::aProcessThatDoesNotExistHasNoSound()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    constexpr quint32 NOBODY = 0x7FFFFFF0;
    QVERIFY(!processExists(NOBODY));

    Recording nobody;
    nobody.capture.start(NOBODY, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(TONE_MS + SOUND_ARRIVES_MS);
    nobody.capture.stop();

    QVERIFY2(nobody.failures.isEmpty(), qPrintable(nobody.failures.join(' ')));
    QVERIFY(nobody.everyFrameIsWhole());
    QVERIFY(nobody.sound().isEmpty());
#ifndef Q_OS_WIN
    QVERIFY2(nobody.frames.size() >= TONE_MS / Audio::AUDIO_FRAME_DURATION_MS - 2, qPrintable(QString::number(nobody.frames.size())));
#endif
}

void TestLoopbackCapture::startingAndStoppingLeavesNoThreadBehind()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    const auto startAndStop = [self] {
        LoopbackCapture capture;
        for (int wait = 0; wait <= 30; wait += 10) {
            capture.start(self, LoopbackCapture::Scope::AllButProcessTree);
            QTest::qWait(wait);
            capture.stop();
        }
        capture.start(self, LoopbackCapture::Scope::ProcessTree);
    };

    startAndStop();
    const int before = threadsOfThisProcess();

    constexpr int ROUNDS = 20;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < ROUNDS; i++)
        startAndStop();
    QVERIFY(timer.elapsed() < 10000);
    QVERIFY2(threadsOfThisProcess() - before < ROUNDS / 2, qPrintable(QStringLiteral("%1 threads before, %2 after").arg(before).arg(threadsOfThisProcess())));
}

void TestLoopbackCapture::thisProgramIsNotAmongThePlayingPrograms()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    Tone tone;
    if (!tone.isPlaying())
        QSKIP(CANNOT_PLAY);
    QTest::qWait(SOUND_ARRIVES_MS);

    QElapsedTimer timer;
    timer.start();
    const QList<Audio::PlayingProgram> programs = Audio::playingPrograms();
    const QList<Audio::PlayingProgram> again = Audio::playingPrograms();
    QVERIFY(timer.elapsed() < 1000);

    QCOMPARE(again.size(), programs.size());
    for (int i = 0; i < programs.size(); i++) {
        QVERIFY(programs[i].processId != self);
        QCOMPARE(again[i].processId, programs[i].processId);
        QCOMPARE(again[i].name, programs[i].name);
        QVERIFY(i == 0 || programs[i - 1].name <= programs[i].name);
        QVERIFY(std::none_of(programs.cbegin(), programs.cbegin() + i, [&](const Audio::PlayingProgram &other) { return other.processId == programs[i].processId; }));
    }
}

#ifdef ACHERON_HAVE_PIPEWIRE
void TestLoopbackCapture::whatALoopbackPlaysAgainIsLeftOut()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    constexpr quint32 NOBODY = 0x7FFFFFF0;
    constexpr int LOOPBACK_DELAY_MS = 100;

    OwnPipeWire own;
    if (!own.running)
        QSKIP(CANNOT_RUN);

    QProcess loopback;
    loopback.setProcessEnvironment(own.environment);
    loopback.setProgram(QStringLiteral("pw-loopback"));
    loopback.setArguments({ QStringLiteral("--delay"), QString::number(LOOPBACK_DELAY_MS / 1000.0), QStringLiteral("--capture-props"), QStringLiteral("media.class=Audio/Sink node.name=chain-in"), QStringLiteral("--playback-props"), QStringLiteral("node.name=chain-out target.object=test-sink") });
    qint64 loopbackId = 0;
    QVERIFY(loopback.startDetached(&loopbackId));
    const auto ended = qScopeGuard([&] { ::kill(static_cast<pid_t>(loopbackId), SIGTERM); });
    const auto links = [&own] { return own.ask(QStringLiteral("pw-link"), { QStringLiteral("-l") }); };
    QVERIFY(QTest::qWaitFor([&] { return links().contains("test-sink:playback_FL\n  |<- chain-out:output_FL"); }, 10000));

    Recording everything;
    Recording chain;
    everything.capture.start(NOBODY, LoopbackCapture::Scope::AllButProcessTree);
    chain.capture.start(static_cast<quint32>(loopbackId), LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    QProcess player;
    own.play(player, toneAsPcm(2), QStringLiteral("FL,FR"), { QStringLiteral("--target"), QStringLiteral("chain-in") });
    QVERIFY(QTest::qWaitFor([&] { return links().contains("chain-in:playback_FL\n  |<- pw-cat:output_FL"); }, 5000));
    const QList<Audio::PlayingProgram> programs = Audio::playingPrograms();
    QVERIFY(std::none_of(programs.cbegin(), programs.cend(), [&](const Audio::PlayingProgram &program) { return program.processId == loopbackId; }));
    QVERIFY(QTest::qWaitFor([&] { return player.state() == QProcess::NotRunning; }, 10000));
    QTest::qWait(LOOPBACK_DELAY_MS + SOUND_ARRIVES_MS);
    everything.capture.stop();
    chain.capture.stop();

    QVERIFY2(everything.failures.isEmpty(), qPrintable(everything.failures.join(' ')));
    QVERIFY2(chain.failures.isEmpty(), qPrintable(chain.failures.join(' ')));
    QVERIFY(everything.everyFrameIsWhole());
    const int framesOfTone = TONE_MS / Audio::AUDIO_FRAME_DURATION_MS;
    const int framesOfSound = static_cast<int>(everything.sound().size()) / Audio::AUDIO_FRAME_SIZE;
    QVERIFY2(framesOfSound >= framesOfTone - 2 && framesOfSound <= framesOfTone + 2, qPrintable(QString::number(framesOfSound)));
    QVERIFY(hasTone(everything.sound()));
    QVERIFY(chain.sound().isEmpty());
}

void TestLoopbackCapture::framesComeEvenlyHoweverLongTheGraphsCycle()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    constexpr quint32 NOBODY = 0x7FFFFFF0;

    OwnPipeWire own;
    if (!own.running)
        QSKIP(CANNOT_RUN);
    // PipeWire's longest cycle: 171 ms of sound at a time
    own.ask(QStringLiteral("pw-metadata"), { QStringLiteral("-n"), QStringLiteral("settings"), QStringLiteral("0"), QStringLiteral("clock.force-quantum"), QStringLiteral("8192") });
    QVERIFY(own.ask(QStringLiteral("pw-metadata"), { QStringLiteral("-n"), QStringLiteral("settings") }).contains("key:'clock.force-quantum' value:'8192'"));

    std::mutex mutex;
    QElapsedTimer clock;
    QList<qint64> soundSentAt;
    QList<qint64> sentAt;
    Recording everything;
    const auto noted = [&](const QByteArray &pcm) {
        const std::lock_guard lock(mutex);
        sentAt.append(clock.elapsed());
        if (!isSilence(pcm))
            soundSentAt.append(sentAt.last());
    };
    connect(&everything.capture, &LoopbackCapture::frameCaptured, &everything.capture, noted, Qt::DirectConnection);
    clock.start();
    everything.capture.start(NOBODY, LoopbackCapture::Scope::AllButProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);

    QProcess player;
    own.play(player, toneAsPcm(2), QStringLiteral("FL,FR"));
    QVERIFY(QTest::qWaitFor([&] { return player.state() == QProcess::NotRunning; }, 10000));
    QTest::qWait(2 * SOUND_ARRIVES_MS);
    everything.capture.stop();

    const std::lock_guard lock(mutex);
    QVERIFY2(everything.failures.isEmpty(), qPrintable(everything.failures.join(' ')));
    const int framesOfTone = TONE_MS / Audio::AUDIO_FRAME_DURATION_MS;
    QVERIFY2(soundSentAt.size() > framesOfTone / 2 && soundSentAt.size() <= framesOfTone + 2, qPrintable(QString::number(soundSentAt.size())));
    qint64 longestWait = 0;
    for (int i = 1; i < sentAt.size(); i++) {
        if (sentAt[i - 1] >= soundSentAt.first() && sentAt[i] <= soundSentAt.last())
            longestWait = qMax(longestWait, sentAt[i] - sentAt[i - 1]);
    }
    {
        QString gaps;
        for (int i = 1; i < sentAt.size(); i++)
            gaps += QString::number(sentAt[i] - sentAt[i - 1]) + ' ';
    }
    QVERIFY2(longestWait < StreamAudio::SOUND_GONE_MS / 2, qPrintable(QString::number(longestWait)));
}

void TestLoopbackCapture::surroundSoundIsMixedDownToBothSides()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    constexpr quint32 NOBODY = 0x7FFFFFF0;
    constexpr double FULL = TONE_AMPLITUDE * 32767;
    const QString surround = QStringLiteral("FL,FR,FC,LFE,RL,RR");

    OwnPipeWire own("FL FR FC LFE RL RR");
    if (!own.running)
        QSKIP(CANNOT_RUN);

    const auto heard = [&](const QByteArray &pcm, const QString &channels, const QStringList &more = {}) {
        Recording everything;
        everything.capture.start(NOBODY, LoopbackCapture::Scope::AllButProcessTree);
        QTest::qWait(CAPTURE_STARTS_MS);
        QProcess player;
        own.play(player, pcm, channels, more);
        const bool played = QTest::qWaitFor([&] { return player.state() == QProcess::NotRunning; }, 10000);
        QTest::qWait(SOUND_ARRIVES_MS);
        everything.capture.stop();
        return played && everything.failures.isEmpty() ? everything.sound() : QByteArray();
    };

    // dialogue is on the centre, which stereo has on both sides at 0.71 of its level
    const QByteArray centre = heard(toneAsPcm(6, surround.split(QLatin1Char(',')).indexOf(QStringLiteral("FC"))), surround);
    for (int side = 0; side < 2; side++) {
        QVERIFY(hasTone(oneSideOf(centre, side)));
        const double part = level(oneSideOf(centre, side), TONE_HZ) / FULL;
        QVERIFY2(part > 0.5 && part < 0.8, qPrintable(QString::number(part)));
    }

    const double ofStereo = level(heard(toneAsPcm(2), QStringLiteral("FL,FR"), { QStringLiteral("--properties"), QStringLiteral("{ channelmix.upmix-method = simple }") }), TONE_HZ) / FULL;
    QVERIFY2(ofStereo > 0.75 && ofStereo < 1.1, qPrintable(QString::number(ofStereo)));
}

void TestLoopbackCapture::startingWithNoFileLeftToOpenFails()
{
    if (!Audio::processLoopbackAvailable())
        QSKIP(CANNOT_CAPTURE);
    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());

    rlimit files = {};
    QCOMPARE(getrlimit(RLIMIT_NOFILE, &files), 0);
    rlimit none = files;
    none.rlim_cur = 0;

    Recording starved;
    QCOMPARE(setrlimit(RLIMIT_NOFILE, &none), 0);
    starved.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QCOMPARE(setrlimit(RLIMIT_NOFILE, &files), 0);
    QTRY_COMPARE_WITH_TIMEOUT(starved.failures.size(), 1, 2000);

    starved.capture.start(self, LoopbackCapture::Scope::ProcessTree);
    QTest::qWait(CAPTURE_STARTS_MS);
    starved.capture.stop();
    QCOMPARE(starved.failures.size(), 1);
}

void TestLoopbackCapture::anAnswerOfNoIsAskedAgain()
{
    if (!qEnvironmentVariableIsSet(ASKING_ALONE)) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QLatin1String(ASKING_ALONE), QStringLiteral("1"));
        QProcess alone;
        alone.setProcessEnvironment(environment);
        alone.setProcessChannelMode(QProcess::MergedChannels);
        alone.start(QCoreApplication::applicationFilePath(), { QStringLiteral("anAnswerOfNoIsAskedAgain") });
        QVERIFY(alone.waitForFinished(60000));
        const QByteArray told = alone.readAll();
        if (told.contains("SKIP "))
            QSKIP(CANNOT_RUN);
        QVERIFY2(alone.exitStatus() == QProcess::NormalExit && alone.exitCode() == 0, told.constData());
        return;
    }

    const QTemporaryDir nowhere;
    qputenv("PIPEWIRE_RUNTIME_DIR", nowhere.path().toLocal8Bit());
    QVERIFY(!Audio::processLoopbackAvailable());
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 1000; i++)
        QVERIFY(!Audio::processLoopbackAvailable());
    QVERIFY(timer.elapsed() < 100);

    OwnPipeWire own;
    if (!own.running)
        QSKIP(CANNOT_RUN);
    QTRY_VERIFY_WITH_TIMEOUT(Audio::processLoopbackAvailable(), 5000);
}
#endif

void TestLoopbackCapture::silenceIsWhatRoundingLeaves()
{
    QVERIFY(isSilence(QByteArray()));
    QVERIFY(isSilence(frameOf(0)));
    QVERIFY(isSilence(frameOf(2)));
    QVERIFY(isSilence(frameOf(-6)));
    QVERIFY(!isSilence(frameOf(7)));
    QVERIFY(!isSilence(frameOf(-7)));

    QByteArray click = frameOf(0);
    reinterpret_cast<qint16 *>(click.data())[Audio::AUDIO_FRAME_SIZE / 2 - 1] = 1000;
    QVERIFY(!isSilence(click));
    QVERIFY(!isSilence(frameOf(-32768)));
}

void TestLoopbackCapture::aRunStartsWithSoundAndOutlastsAShortSilence()
{
    SoundRuns runs;
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Continue);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(true), SoundRuns::Step::End);
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
}

void TestLoopbackCapture::soundDuringTheSilenceKeepsTheRunOpen()
{
    SoundRuns runs;
    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Continue);

    for (int i = 0; i < SoundRuns::HELD_SILENT_FRAMES; i++)
        QCOMPARE(runs.offer(true), SoundRuns::Step::Continue);
    QCOMPARE(runs.offer(true), SoundRuns::Step::End);
}

void TestLoopbackCapture::aRunEndsWhenFramesStopComing()
{
    SoundRuns runs;
    QVERIFY(!runs.stop());

    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
    QVERIFY(runs.stop());
    QVERIFY(!runs.stop());
    QCOMPARE(runs.offer(true), SoundRuns::Step::Skip);
    QCOMPARE(runs.offer(false), SoundRuns::Step::Begin);
}

QTEST_GUILESS_MAIN(TestLoopbackCapture)
#include "tst_LoopbackCapture.moc"
