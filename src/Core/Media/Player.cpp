#include "Core/Media/Player.hpp"

#include "Core/Logging.hpp"

#include <QFileInfo>
#include <QSettings>

#include <initializer_list>
#include <utility>

#ifdef ACHERON_HAVE_FFMPEG

#include "Core/Media/AudioOutput.hpp"
#include "Core/Media/MediaDecoder.hpp"
#include "Core/Media/PacketQueue.hpp"

#include <QElapsedTimer>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#endif // ACHERON_HAVE_FFMPEG

namespace Acheron {
namespace Core {
namespace Media {

namespace {

constexpr auto VolumeKey = "media/volume";
constexpr auto MutedKey = "media/muted";

bool listContains(const QString &value, std::initializer_list<const char *> list)
{
    for (const char *item : list) {
        if (value == QLatin1String(item))
            return true;
    }
    return false;
}

QString normalizedContentType(const QString &contentType)
{
    return contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
}

bool hasSuffixIn(const QUrl &url, std::initializer_list<const char *> suffixes)
{
    return listContains(QFileInfo(url.path()).suffix().toLower(), suffixes);
}

} // namespace

bool isSupported()
{
#ifdef ACHERON_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

bool canPlay(const QString &contentType, const QUrl &url)
{
    if (!isSupported())
        return false;

    if (listContains(normalizedContentType(contentType), { "video/mp4", "video/webm", "video/quicktime", "video/x-m4v" }))
        return true;

    return hasSuffixIn(url, { "mp4", "webm", "mov", "m4v" });
}

bool canPlayAudio(const QString &contentType, const QUrl &url)
{
    if (!isSupported())
        return false;

    if (normalizedContentType(contentType).startsWith(QLatin1String("audio/")))
        return true;

    return hasSuffixIn(url, { "mp3", "ogg", "oga", "opus", "wav", "flac", "m4a", "aac" });
}

float storedVolume()
{
    const QVariant stored = QSettings().value(VolumeKey);
    return stored.isValid() ? qBound(0.0f, stored.toFloat(), 1.0f) : 1.0f;
}

void setStoredVolume(float volume)
{
    QSettings().setValue(VolumeKey, qBound(0.0f, volume, 1.0f));
}

bool storedMuted()
{
    return QSettings().value(MutedKey, false).toBool();
}

void setStoredMuted(bool muted)
{
    QSettings().setValue(MutedKey, muted);
}

struct OwnerThreadState
{
    Player *owner = nullptr;

    Player::State state = Player::State::Idle;
    qint64 durationMs = 0;
    qint64 positionMs = 0;
    QSize native;
    bool audioAvailable = false;
    bool videoAvailable = false;
    bool seekable = false;
    QImage frame;

    float userVolume = storedVolume();
    bool userMuted = storedMuted();

    void setState(Player::State next)
    {
        if (state == next)
            return;
        state = next;
        emit owner->stateChanged(next);
    }
};

#ifdef ACHERON_HAVE_FFMPEG

namespace {

constexpr int MaxQueuedFrames = 4;
constexpr int AudioChunkFrames = 2048;

constexpr qint64 MinPresentDelayMs = 1;
constexpr qint64 MaxPresentDelayMs = 40;

constexpr int AudioOnlyPresentDelayMs = 100;

QSize effectiveTarget(const QSize &native, const QSize &want)
{
    if (native.isEmpty())
        return native;
    if (want.isEmpty())
        return native;

    QSize scaled = native;
    scaled.scale(want, Qt::KeepAspectRatio);
    if (scaled.width() >= native.width() || scaled.height() >= native.height())
        return native;

    return scaled.isEmpty() ? native : scaled;
}

} // namespace

struct Player::Impl : OwnerThreadState
{
    explicit Impl(Player *player)
    {
        owner = player;
        presentTimer.setSingleShot(true);
        presentTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&presentTimer, &QTimer::timeout, player, [this] { present(); });
    }

    struct DecodedFrame
    {
        QImage image;
        qint64 ptsMs = 0;
    };

    // owner thread
    QTimer presentTimer;
    QElapsedTimer wallTimer;
    qint64 wallBaseMs = 0;
    bool seeking = false;
    bool audioClockStalled = false;
    bool autoPlayOnReady = false;
    // deleting this cancels whatever the worker threads have already posted
    QObject *sessionContext = nullptr;
    std::thread worker;
    std::thread demuxer;

    // decode thread
    MediaDecoder decoder;
    std::vector<float> audioScratch;
    std::vector<float> audioPending;
    size_t audioPendingOffset = 0;

    // shared, guarded by mutex, waited on through cv
    std::mutex mutex;
    std::condition_variable cv;
    PacketQueue packets;
    std::deque<DecodedFrame> frames;
    QSize targetSize;

    // shared, lock free
    std::atomic<bool> running{ false };
    std::atomic<bool> paused{ true };
    std::atomic<bool> reachedEnd{ false };
    std::atomic<bool> audioSinkBroken{ false };
    std::atomic<bool> primed{ false };
    std::atomic<qint64> pendingSeek{ -1 };
    std::atomic<qint64> seekTargetMs{ -1 };
    std::atomic<bool> awaitingSeekFrame{ false };
    std::atomic<qint64> demuxedMs{ 0 };
    std::atomic<bool> demuxEof{ false };
    std::atomic<bool> flushPending{ false };

    AudioOutput audio;

    void open(const QUrl &url);
    void close();
    void play();
    void pause();
    void seek(qint64 positionMs);
    void setVolume(float volume);
    void setMuted(bool muted);
    void setTargetSize(const QSize &size);
    [[nodiscard]] qint64 bufferedPosition() const { return demuxedMs.load(std::memory_order_relaxed); }

    void startWorker(const QString &input);
    void stopWorker();
    void present();
    void scheduleNextPresent(qint64 clock);
    [[nodiscard]] qint64 clockMs() const;
    void fail(const QString &message);

    // decode/demux threads
    static int interruptThunk(void *opaque);
    void decodeLoop(QString input);
    void demuxLoop();
    bool openSource(const QString &input);
    void demuxSeek(qint64 targetMs);
    void applyFlush(qint64 targetMs);
    void drainAtEnd(AVFrame *avFrame);
    void decodePacket(AVPacket *pkt, AVFrame *avFrame);
    bool pushVideoFrame(AVFrame *avFrame);
    void pushAudioFrame(AVFrame *avFrame);
    void drainPendingAudio();

    [[nodiscard]] bool beforeSeekTarget(qint64 ptsMs) const
    {
        const qint64 target = seekTargetMs.load(std::memory_order_relaxed);
        return target >= 0 && ptsMs < target;
    }

    [[nodiscard]] bool demuxInterrupted() const
    {
        return !running.load(std::memory_order_relaxed) || pendingSeek.load(std::memory_order_relaxed) >= 0;
    }

    [[nodiscard]] bool seekInFlight() const
    {
        return pendingSeek.load(std::memory_order_relaxed) >= 0 || flushPending.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool interrupted() const
    {
        return !running.load(std::memory_order_relaxed) || seekInFlight();
    }

    [[nodiscard]] bool audioPendingEmpty() const { return audioPendingOffset >= audioPending.size(); }

    [[nodiscard]] qint64 audioDrainWaitMs() const
    {
        const qint64 buffered = qint64(audio.bufferedFrames()) * 1000 / AudioOutput::SampleRate;
        return qBound(qint64(5), buffered / 2, qint64(250));
    }

    [[nodiscard]] bool audioSinkDrained() const
    {
        if (!audioSinkUsable())
            return true;
        if (!audioPendingEmpty())
            return false;
        return audio.bufferedFrames() == 0 ||
               (!audio.isRunning() && !paused.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool audioSinkUsable() const
    {
        return decoder.hasAudio() && !audioSinkBroken.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool decodable() const
    {
        return (!paused.load(std::memory_order_relaxed) && !reachedEnd.load(std::memory_order_relaxed)) ||
               !primed.load(std::memory_order_relaxed);
    }

    void clearPendingAudio()
    {
        audioPending.clear();
        audioPendingOffset = 0;
    }

    template <typename F>
    void postToOwner(F &&fn)
    {
        QMetaObject::invokeMethod(sessionContext, std::forward<F>(fn));
    }
};

#else // ACHERON_HAVE_FFMPEG

struct Player::Impl : OwnerThreadState
{
    explicit Impl(Player *player) { owner = player; }

    void open(const QUrl &) {}
    void close() {}
    void play() {}
    void pause() {}
    void seek(qint64) {}
    void setVolume(float value) { userVolume = qBound(0.0f, value, 1.0f); }
    void setMuted(bool value) { userMuted = value; }
    void setTargetSize(const QSize &) {}
    [[nodiscard]] qint64 bufferedPosition() const { return 0; }
};

#endif // ACHERON_HAVE_FFMPEG

Player::Player(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}

Player::~Player()
{
    d->state = State::Idle; // no stateChanged() out of a destructor
    d->close();
}

void Player::open(const QUrl &url)
{
    d->open(url);
}

void Player::close()
{
    d->close();
}

void Player::play()
{
    d->play();
}

void Player::pause()
{
    d->pause();
}

void Player::togglePlayPause()
{
    if (state() == State::Playing)
        pause();
    else
        play();
}

void Player::seek(qint64 positionMs)
{
    d->seek(positionMs);
}

void Player::setVolume(float volume)
{
    d->setVolume(volume);
}

float Player::volume() const
{
    return d->userVolume;
}

void Player::setMuted(bool muted)
{
    d->setMuted(muted);
}

bool Player::isMuted() const
{
    return d->userMuted;
}

void Player::setTargetSize(const QSize &size)
{
    d->setTargetSize(size);
}

QImage Player::currentFrame() const
{
    return d->frame;
}

qint64 Player::position() const
{
    return d->positionMs;
}

qint64 Player::duration() const
{
    return d->durationMs;
}

qint64 Player::bufferedPosition() const
{
    return d->bufferedPosition();
}

QSize Player::nativeSize() const
{
    return d->native;
}

bool Player::hasVideo() const
{
    return d->videoAvailable;
}

Player::State Player::state() const
{
    return d->state;
}

bool Player::isSeekable() const
{
    return d->seekable;
}

#ifdef ACHERON_HAVE_FFMPEG

void Player::Impl::open(const QUrl &url)
{
    close();

    setState(Player::State::Opening);

    startWorker(url.isLocalFile() ? url.toLocalFile() : url.toString());
}

void Player::Impl::close()
{
    stopWorker();

    delete sessionContext;
    sessionContext = nullptr;

    presentTimer.stop();
    audio.stop();

    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
    }

    positionMs = 0;
    durationMs = 0;
    demuxedMs.store(0, std::memory_order_relaxed);
    frame = QImage();
    audioAvailable = false;
    videoAvailable = false;
    seeking = false;
    audioClockStalled = false;
    autoPlayOnReady = false;

    setState(Player::State::Idle);
}

void Player::Impl::play()
{
    if (state == Player::State::Opening) {
        autoPlayOnReady = true;
        return;
    }

    if (state != Player::State::Paused && state != Player::State::Ended)
        return;

    if (state == Player::State::Ended)
        seek(0);

    if (audioAvailable) {
        const bool started = audio.start();
        audioSinkBroken.store(!started, std::memory_order_relaxed);
        if (!started)
            audioClockStalled = true;
    }

    wallBaseMs = positionMs;
    wallTimer.restart();

    {
        std::lock_guard<std::mutex> lock(mutex);
        paused.store(false, std::memory_order_relaxed);
    }
    cv.notify_all();

    setState(Player::State::Playing);
    scheduleNextPresent(positionMs);
}

void Player::Impl::pause()
{
    if (state != Player::State::Playing)
        return;

    wallBaseMs = clockMs();
    {
        std::lock_guard<std::mutex> lock(mutex);
        paused.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();

    audio.stop();
    presentTimer.stop();

    setState(Player::State::Paused);
}

void Player::Impl::seek(qint64 requestedMs)
{
    if (!running.load(std::memory_order_relaxed))
        return;

    if (state == Player::State::Opening)
        return;

    const qint64 upper = durationMs > 0 ? durationMs : requestedMs;
    const qint64 target = qBound(qint64(0), requestedMs, upper);

    seeking = true;
    positionMs = target;
    wallBaseMs = target;
    wallTimer.restart();
    audioClockStalled = false;
    reachedEnd.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingSeek.store(target, std::memory_order_relaxed);
    }
    cv.notify_all();

    emit owner->positionChanged(target);

    if (state == Player::State::Ended)
        setState(Player::State::Paused);
}

void Player::Impl::setVolume(float value)
{
    const float bounded = qBound(0.0f, value, 1.0f);
    if (qFuzzyCompare(bounded + 1.0f, userVolume + 1.0f))
        return;

    userVolume = bounded;
    setStoredVolume(bounded);
    audio.setVolume(bounded);
}

void Player::Impl::setMuted(bool value)
{
    if (userMuted == value)
        return;

    userMuted = value;
    setStoredMuted(value);
    audio.setMuted(value);
}

void Player::Impl::setTargetSize(const QSize &size)
{
    std::lock_guard<std::mutex> lock(mutex);
    targetSize = size;
}

void Player::Impl::fail(const QString &message)
{
    qCWarning(LogVideo) << "playback failed:" << message;
    setState(Player::State::Error);
    emit owner->errorOccurred(message);
}

void Player::Impl::startWorker(const QString &input)
{
    stopWorker();

    audio.setVolume(userVolume);
    audio.setMuted(userMuted);

    delete sessionContext;
    sessionContext = new QObject(owner);
    running.store(true, std::memory_order_relaxed);
    paused.store(true, std::memory_order_relaxed);
    reachedEnd.store(false, std::memory_order_relaxed);
    pendingSeek.store(-1, std::memory_order_relaxed);
    seekTargetMs.store(-1, std::memory_order_relaxed);
    awaitingSeekFrame.store(false, std::memory_order_relaxed);
    demuxedMs.store(0, std::memory_order_relaxed);
    demuxEof.store(false, std::memory_order_relaxed);
    flushPending.store(false, std::memory_order_relaxed);
    audioSinkBroken.store(false, std::memory_order_relaxed);
    primed.store(false, std::memory_order_relaxed);

    worker = std::thread(&Player::Impl::decodeLoop, this, input);
}

void Player::Impl::stopWorker()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        running.store(false, std::memory_order_relaxed);
    }

    if (worker.joinable()) {
        cv.notify_all();
        worker.join();
    }
}

int Player::Impl::interruptThunk(void *opaque)
{
    auto *impl = static_cast<Player::Impl *>(opaque);
    return impl && impl->demuxInterrupted() ? 1 : 0;
}

bool Player::Impl::openSource(const QString &input)
{
    decoder.setInterrupt(&Player::Impl::interruptThunk, this);

    QString openError;
    if (!decoder.open(input, &openError)) {
        postToOwner([this, openError] { fail(openError); });
        return false;
    }

    const MediaDecoder::Info info = decoder.info();
    if (!info.hasVideo)
        primed.store(true, std::memory_order_relaxed);

    postToOwner([this, info] {
        native = info.native;
        durationMs = info.durationMs;
        seekable = info.seekable;
        audioAvailable = info.hasAudio;
        videoAvailable = info.hasVideo;
        emit owner->durationChanged(info.durationMs);
    });

    return true;
}

void Player::Impl::decodePacket(AVPacket *pkt, AVFrame *avFrame)
{
    const MediaDecoder::Stream stream = decoder.streamOf(pkt);
    if (stream == MediaDecoder::Stream::None || !decoder.sendPacket(stream, pkt))
        return;

    while (decoder.receiveFrame(stream, avFrame) >= 0) {
        if (stream == MediaDecoder::Stream::Video)
            pushVideoFrame(avFrame);
        else
            pushAudioFrame(avFrame);
        av_frame_unref(avFrame);
    }
}

void Player::Impl::demuxSeek(qint64 targetMs)
{
    QString seekError;
    if (!decoder.seek(targetMs, &seekError))
        qCWarning(LogVideo) << "seek failed:" << seekError;

    demuxEof.store(false, std::memory_order_relaxed);
    demuxedMs.store(targetMs, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mutex);
        packets.clearLocked();
        packets.pushFlushLocked(targetMs);
        flushPending.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();
}

void Player::Impl::applyFlush(qint64 targetMs)
{
    decoder.flush();

    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
    }
    clearPendingAudio();

    audio.reset(targetMs);

    reachedEnd.store(false, std::memory_order_relaxed);
    seekTargetMs.store(targetMs, std::memory_order_relaxed);

    if (decoder.hasVideo()) {
        awaitingSeekFrame.store(true, std::memory_order_relaxed);
        primed.store(false, std::memory_order_relaxed);
        return;
    }

    postToOwner([this, targetMs] {
        seeking = false;
        audioClockStalled = false;
        wallBaseMs = targetMs;
        wallTimer.restart();
        if (state == Player::State::Playing)
            scheduleNextPresent(targetMs);
    });
}

void Player::Impl::demuxLoop()
{
    AVPacket *packet = av_packet_alloc();

    while (running.load(std::memory_order_relaxed)) {
        const qint64 seekTarget = pendingSeek.exchange(-1, std::memory_order_relaxed);
        if (seekTarget >= 0) {
            demuxSeek(seekTarget);
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (packets.fullLocked() || demuxEof.load(std::memory_order_relaxed)) {
                cv.wait(lock, [this] {
                    return demuxInterrupted() ||
                           (!demuxEof.load(std::memory_order_relaxed) && !packets.fullLocked());
                });
                continue;
            }
        }

        const int ret = decoder.readPacket(packet);
        if (ret < 0) {
            if (demuxInterrupted())
                continue;

            if (ret == AVERROR(EAGAIN)) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }

            if (ret != AVERROR_EOF)
                qCWarning(LogVideo) << "read failed:" << MediaDecoder::errorString(ret);

            const qint64 total = decoder.containerDurationMs();
            if (total > 0)
                demuxedMs.store(total, std::memory_order_relaxed);
            demuxEof.store(true, std::memory_order_relaxed);
            cv.notify_all();
            continue;
        }

        if (decoder.streamOf(packet) == MediaDecoder::Stream::None) {
            av_packet_unref(packet);
            continue;
        }

        const qint64 tsMs = decoder.packetTimestampMs(packet);
        if (tsMs >= 0 && tsMs > demuxedMs.load(std::memory_order_relaxed))
            demuxedMs.store(tsMs, std::memory_order_relaxed);

        AVPacket *stored = av_packet_alloc();
        av_packet_move_ref(stored, packet);

        bool wasEmpty;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wasEmpty = packets.emptyLocked();
            packets.pushLocked(stored);
        }

        if (wasEmpty)
            cv.notify_all();
    }

    av_packet_free(&packet);
}

bool Player::Impl::pushVideoFrame(AVFrame *avFrame)
{
    const qint64 ptsMs = decoder.frameTimestampMs(avFrame, MediaDecoder::Stream::Video);

    if (beforeSeekTarget(ptsMs))
        return false;

    QSize want;
    {
        std::lock_guard<std::mutex> lock(mutex);
        want = targetSize;
    }

    const QImage image = decoder.scaleToImage(avFrame, effectiveTarget(QSize(avFrame->width, avFrame->height), want));
    if (image.isNull())
        return false;

    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wasEmpty = frames.empty();
        frames.push_back({ image, ptsMs });
    }
    primed.store(true, std::memory_order_relaxed);

    const bool resolvedSeek = awaitingSeekFrame.exchange(false, std::memory_order_relaxed);

    if (!resolvedSeek && !wasEmpty)
        return true;

    postToOwner([this, ptsMs, resolvedSeek, image] {
        if (resolvedSeek) {
            seeking = false;
            audioClockStalled = false;
            wallBaseMs = ptsMs;
            wallTimer.restart();
        }

        if (state == Player::State::Playing) {
            if (!presentTimer.isActive())
                present();
            return;
        }

        if (!frame.isNull() && !resolvedSeek)
            return;

        frame = image;
        if (resolvedSeek && positionMs != ptsMs) {
            positionMs = ptsMs;
            emit owner->positionChanged(positionMs);
        }
        emit owner->frameReady();
    });

    return true;
}

void Player::Impl::pushAudioFrame(AVFrame *avFrame)
{
    if (!audioSinkUsable())
        return;

    if (beforeSeekTarget(decoder.frameTimestampMs(avFrame, MediaDecoder::Stream::Audio)))
        return;

    if (decoder.appendResampledFrames(avFrame, audioPending, audioScratch) <= 0)
        return;

    drainPendingAudio();
}

void Player::Impl::drainPendingAudio()
{
    if (audioPendingEmpty())
        return;

    if (audioSinkBroken.load(std::memory_order_relaxed)) {
        clearPendingAudio();
        return;
    }

    const size_t available = audioPending.size() - audioPendingOffset;
    const int pendingFrames = static_cast<int>(available / AudioOutput::Channels);
    const int written = audio.write(audioPending.data() + audioPendingOffset, pendingFrames);
    if (written <= 0)
        return;

    audioPendingOffset += static_cast<size_t>(written) * AudioOutput::Channels;
    if (audioPendingEmpty())
        clearPendingAudio();
}

void Player::Impl::decodeLoop(QString input)
{
    if (!openSource(input)) {
        decoder.close();
        return;
    }

    postToOwner([this] {
        if (state != Player::State::Opening)
            return;
        setState(Player::State::Paused);
        if (autoPlayOnReady) {
            autoPlayOnReady = false;
            play();
        }
    });

    demuxer = std::thread(&Player::Impl::demuxLoop, this);

    AVFrame *avFrame = av_frame_alloc();

    while (running.load(std::memory_order_relaxed)) {
        drainPendingAudio();

        AVPacket *next = nullptr;
        qint64 flushTarget = -1;
        bool starved = false;
        bool queueWasFull = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (packets.emptyLocked()) {
                starved = true;
            } else if (packets.frontIsFlushLocked()) {
                flushTarget = packets.takeFlushLocked();
                flushPending.store(false, std::memory_order_relaxed);
            } else if (decodable()) {
                const bool videoRoom = decoder.hasVideo() && frames.size() < MaxQueuedFrames;
                const bool audioRoom = audioSinkUsable() &&
                                       audio.writableFrames() >= AudioChunkFrames &&
                                       audioPendingEmpty();
                if (videoRoom || audioRoom) {
                    queueWasFull = packets.fullLocked();
                    next = packets.takePacketLocked();
                }
            }
        }

        if (flushTarget >= 0) {
            applyFlush(flushTarget);
            continue;
        }

        if (next) {
            if (queueWasFull)
                cv.notify_all();

            decodePacket(next, avFrame);
            av_packet_free(&next);
            continue;
        }

        if (starved &&
            demuxEof.load(std::memory_order_relaxed) &&
            !reachedEnd.load(std::memory_order_relaxed) &&
            !seekInFlight()) {
            drainAtEnd(avFrame);
            continue;
        }

        std::unique_lock<std::mutex> lock(mutex);
        if (decodable() && !packets.emptyLocked() && audioSinkUsable()) {
            cv.wait_for(lock, std::chrono::milliseconds(audioDrainWaitMs()));
        } else {
            cv.wait(lock, [this] {
                if (!running.load(std::memory_order_relaxed))
                    return true;

                if (packets.emptyLocked())
                    return demuxEof.load(std::memory_order_relaxed) &&
                           !reachedEnd.load(std::memory_order_relaxed) &&
                           !seekInFlight();

                if (packets.frontIsFlushLocked())
                    return true;

                return decodable() && (!decoder.hasVideo() || frames.size() < MaxQueuedFrames);
            });
        }
    }

    av_frame_free(&avFrame);

    if (demuxer.joinable()) {
        cv.notify_all();
        demuxer.join();
    }

    decoder.close();
}

void Player::Impl::drainAtEnd(AVFrame *avFrame)
{
    if (decoder.drain(MediaDecoder::Stream::Video)) {
        while (!interrupted()) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] {
                    return interrupted() || frames.size() < MaxQueuedFrames;
                });
            }
            if (interrupted() || decoder.receiveFrame(MediaDecoder::Stream::Video, avFrame) < 0)
                break;

            pushVideoFrame(avFrame);
            av_frame_unref(avFrame);
        }
    }

    if (decoder.drain(MediaDecoder::Stream::Audio)) {
        while (!interrupted() && decoder.receiveFrame(MediaDecoder::Stream::Audio, avFrame) >= 0) {
            pushAudioFrame(avFrame);
            av_frame_unref(avFrame);
        }
    }

    bool clockHandedOver = false;
    while (!interrupted()) {
        drainPendingAudio();

        bool videoDrained;
        {
            std::lock_guard<std::mutex> lock(mutex);
            videoDrained = frames.empty();
        }

        const bool audioDrained = audioSinkDrained();

        if (audioDrained && !clockHandedOver && audioSinkUsable()) {
            clockHandedOver = true;
            postToOwner([this] {
                audioClockStalled = true;
                if (state == Player::State::Playing) {
                    wallBaseMs = audio.clockMs();
                    wallTimer.restart();
                }
            });
        }

        if (videoDrained && audioDrained)
            break;

        std::unique_lock<std::mutex> lock(mutex);
        if (paused.load(std::memory_order_relaxed)) {
            cv.wait(lock, [this] {
                return interrupted() || !paused.load(std::memory_order_relaxed);
            });
        } else {
            cv.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    if (interrupted())
        return;

    awaitingSeekFrame.store(false, std::memory_order_relaxed);
    reachedEnd.store(true, std::memory_order_relaxed);
    postToOwner([this] {
        seeking = false;
        if (state == Player::State::Playing) {
            positionMs = durationMs;
            emit owner->positionChanged(positionMs);
            setState(Player::State::Ended);
            presentTimer.stop();
            audio.stop();
        }
    });
}

qint64 Player::Impl::clockMs() const
{
    if (audioAvailable && !audioClockStalled)
        return audio.clockMs();
    if (paused.load(std::memory_order_relaxed))
        return wallBaseMs;
    return wallBaseMs + wallTimer.elapsed();
}

void Player::Impl::scheduleNextPresent(qint64 clock)
{
    if (state != Player::State::Playing)
        return;

    if (!videoAvailable) {
        presentTimer.start(AudioOnlyPresentDelayMs);
        return;
    }

    qint64 delay = 16;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!frames.empty())
            delay = frames.front().ptsMs - clock;
    }

    presentTimer.start(static_cast<int>(qBound(MinPresentDelayMs, delay, MaxPresentDelayMs)));
}

void Player::Impl::present()
{
    if (seeking) {
        scheduleNextPresent(positionMs);
        return;
    }

    const qint64 clock = clockMs();

    QImage latest;
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(mutex);

        while (!frames.empty() && frames.front().ptsMs <= clock) {
            latest = frames.front().image;
            popped = true;
            frames.pop_front();
        }
    }
    cv.notify_all();

    if (popped) {
        frame = latest;
        emit owner->frameReady();
    }

    const qint64 bounded = durationMs > 0 ? qBound(qint64(0), clock, durationMs)
                                          : qMax(qint64(0), clock);
    if (positionMs != bounded) {
        positionMs = bounded;
        emit owner->positionChanged(positionMs);
    }

    scheduleNextPresent(clock);
}

#endif // ACHERON_HAVE_FFMPEG

} // namespace Media
} // namespace Core
} // namespace Acheron
