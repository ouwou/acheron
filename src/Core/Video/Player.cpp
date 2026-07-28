#include "Core/Video/Player.hpp"

#include "Core/Logging.hpp"

#include <QFileInfo>

#ifdef ACHERON_HAVE_FFMPEG

#  include "Core/Video/AudioOutput.hpp"
#  include "Discord/CurlUtils.hpp"

#  include <QElapsedTimer>
#  include <QTimer>

extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libavutil/channel_layout.h>
#  include <libavutil/imgutils.h>
#  include <libavutil/opt.h>
#  include <libswresample/swresample.h>
#  include <libswscale/swscale.h>
}

#  if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57, 28, 100)
#    error "ffmpeg 5.1 or newer is required for video support"
#  endif

#  include <chrono>
#  include <condition_variable>
#  include <deque>
#  include <mutex>
#  include <thread>
#  include <vector>

#endif // ACHERON_HAVE_FFMPEG

namespace Acheron {
namespace Core {
namespace Video {

bool isSupported()
{
#ifdef ACHERON_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

static bool hasPlayableSuffix(const QUrl &url)
{
    const QString suffix = QFileInfo(url.path()).suffix().toLower();
    return suffix == "mp4" ||
           suffix == "webm" ||
           suffix == "mov" ||
           suffix == "m4v";
}

bool canPlay(const QString &contentType, const QUrl &url)
{
    if (!isSupported())
        return false;

    if (!contentType.isEmpty()) {
        const QString type = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        return type == "video/mp4" ||
               type == "video/webm" ||
               type == "video/quicktime" ||
               type == "video/x-m4v";
    }

    return hasPlayableSuffix(url);
}

#ifdef ACHERON_HAVE_FFMPEG

namespace {

constexpr int MaxQueuedFrames = 4;
constexpr int AudioChunkFrames = 2048;

constexpr int MaxDecoderThreads = 2;

constexpr size_t MaxBufferedBytes = 8 * 1024 * 1024;

constexpr qint64 MinPresentDelayMs = 1;
constexpr qint64 MaxPresentDelayMs = 40;

QSize effectiveTarget(const QSize &native, const QSize &want)
{
    if (!native.isValid() || native.isEmpty())
        return native;
    if (!want.isValid() || want.isEmpty())
        return native;

    QSize scaled = native;
    scaled.scale(want, Qt::KeepAspectRatio);
    if (scaled.width() >= native.width() || scaled.height() >= native.height())
        return native;

    return scaled.isEmpty() ? native : scaled;
}

QString averrorString(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

} // namespace

#endif // ACHERON_HAVE_FFMPEG

struct Player::Impl
{
    Player *owner = nullptr;

    Player::State state = Player::State::Idle;
    QString error;
    QUrl source;
    qint64 durationMs = 0;
    qint64 positionMs = 0;
    QSize native;
    bool audioAvailable = false;
    bool videoAvailable = false;
    bool seekable = false;
    bool audioClockStalled = false;
    QImage frame;

#ifdef ACHERON_HAVE_FFMPEG
    struct DecodedFrame
    {
        QImage image;
        qint64 ptsMs = 0;
    };

    std::unique_ptr<AudioOutput> audio;
    QTimer *presentTimer = nullptr;

    QElapsedTimer wallTimer;
    qint64 wallBaseMs = 0;
    bool seeking = false;

    std::thread worker;
    std::thread demuxer;
    std::atomic<bool> running{ false };
    std::atomic<bool> paused{ true };
    std::atomic<bool> reachedEnd{ false };
    std::atomic<bool> audioSinkBroken{ false };
    std::atomic<bool> primed{ false };
    std::atomic<qint64> pendingSeek{ -1 };

    std::atomic<qint64> seekTargetMs{ -1 };
    std::atomic<bool> awaitingSeekFrame{ false };

    std::atomic<qint64> demuxedMs{ 0 };

    bool autoPlayOnReady = false;

    QObject *sessionContext = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<DecodedFrame> frames;
    QSize targetSize;

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *videoCtx = nullptr;
    AVCodecContext *audioCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    QSize swsTarget;

    struct QueuedPacket
    {
        AVPacket *packet = nullptr;
        qint64 flushToMs = -1;
    };
    std::deque<QueuedPacket> packetQueue;
    size_t packetQueueBytes = 0;
    std::atomic<bool> demuxEof{ false };
    std::atomic<bool> flushPending{ false };
    std::vector<float> audioScratch;
    std::vector<float> audioPending;
    size_t audioPendingOffset = 0;

    static int interruptCallback(void *opaque);

    void startWorker(const QString &input);
    void stopWorker();
    void decodeLoop(QString input);
    void demuxLoop();
    bool openInput(const QString &input);
    void teardownInput();
    void demuxSeek(qint64 targetMs);
    void applyFlush(qint64 targetMs);
    void drainAtEnd(AVFrame *avFrame);
    void decodePacket(AVPacket *pkt, AVFrame *avFrame);
    void clearPacketQueueLocked();
    [[nodiscard]] qint64 framePtsMs(const AVFrame *avFrame, int streamIndex) const;

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

    [[nodiscard]] bool audioSinkUsable() const
    {
        return audioStream >= 0 && !audioSinkBroken.load(std::memory_order_relaxed);
    }

    void clearPendingAudio()
    {
        audioPending.clear();
        audioPendingOffset = 0;
    }

    bool pushVideoFrame(AVFrame *avFrame);
    void pushAudioFrame(AVFrame *avFrame);
    void drainPendingAudio();
    void present();
    void scheduleNextPresent(qint64 clock);
    [[nodiscard]] qint64 clockMs() const;

    void setState(Player::State next);
    void fail(const QString &message);

    template <typename F>
    void postToOwner(F &&fn)
    {
        QMetaObject::invokeMethod(sessionContext, std::forward<F>(fn));
    }
#endif // ACHERON_HAVE_FFMPEG
};

Player::Player(QObject *parent) : QObject(parent), d(std::make_unique<Impl>())
{
    d->owner = this;

#ifdef ACHERON_HAVE_FFMPEG
    d->presentTimer = new QTimer(this);
    d->presentTimer->setSingleShot(true);
    d->presentTimer->setTimerType(Qt::PreciseTimer);
    connect(d->presentTimer, &QTimer::timeout, this, [this] { d->present(); });
#endif
}

Player::~Player()
{
    d->state = State::Idle;
    close();
}

void Player::open(const QUrl &url)
{
#ifdef ACHERON_HAVE_FFMPEG
    close();

    d->source = url;
    d->setState(State::Opening);

    d->startWorker(url.isLocalFile() ? url.toLocalFile() : url.toString());
#else
    Q_UNUSED(url);
#endif
}

void Player::close()
{
#ifdef ACHERON_HAVE_FFMPEG
    d->stopWorker();

    delete d->sessionContext;
    d->sessionContext = nullptr;

    if (d->presentTimer)
        d->presentTimer->stop();
    if (d->audio)
        d->audio->stop();

    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->frames.clear();
    }

    d->positionMs = 0;
    d->durationMs = 0;
    d->demuxedMs.store(0, std::memory_order_relaxed);
    d->frame = QImage();
    d->audioAvailable = false;
    d->videoAvailable = false;
    d->seeking = false;
    d->audioClockStalled = false;
    d->autoPlayOnReady = false;

    d->setState(State::Idle);
#endif
}

void Player::play()
{
#ifdef ACHERON_HAVE_FFMPEG
    if (d->state == State::Opening) {
        d->autoPlayOnReady = true;
        return;
    }

    if (d->state != State::Paused && d->state != State::Ended)
        return;

    if (d->state == State::Ended) {
        if (d->audio)
            d->audio->reset(0);
        seek(0);
    }

    if (d->audio && d->audioAvailable) {
        const bool started = d->audio->start();
        d->audioSinkBroken.store(!started, std::memory_order_relaxed);
        if (!started)
            d->audioClockStalled = true;
    }

    d->wallBaseMs = d->positionMs;
    d->wallTimer.restart();

    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->paused.store(false, std::memory_order_relaxed);
    }
    d->cv.notify_all();

    d->setState(State::Playing);
    d->scheduleNextPresent(d->positionMs);
#endif
}

void Player::pause()
{
#ifdef ACHERON_HAVE_FFMPEG
    if (d->state != State::Playing)
        return;

    d->wallBaseMs = d->clockMs();
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->paused.store(true, std::memory_order_relaxed);
    }
    d->cv.notify_all();

    if (d->audio)
        d->audio->stop();
    if (d->presentTimer)
        d->presentTimer->stop();

    d->setState(State::Paused);
#endif
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
#ifdef ACHERON_HAVE_FFMPEG
    if (!d->running.load(std::memory_order_relaxed))
        return;

    if (d->state == State::Opening)
        return;

    const qint64 upper = d->durationMs > 0 ? d->durationMs : positionMs;
    const qint64 target = qBound(qint64(0), positionMs, upper);

    d->seeking = true;
    d->positionMs = target;
    d->wallBaseMs = target;
    d->wallTimer.restart();
    d->audioClockStalled = false;
    d->reachedEnd.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pendingSeek.store(target, std::memory_order_relaxed);
    }
    d->cv.notify_all();

    emit positionChanged(target);

    if (d->state == State::Ended)
        d->setState(State::Paused);
#else
    Q_UNUSED(positionMs);
#endif
}

void Player::setVolume(float volume)
{
#ifdef ACHERON_HAVE_FFMPEG
    if (d->audio)
        d->audio->setVolume(volume);
#else
    Q_UNUSED(volume);
#endif
}

float Player::volume() const
{
#ifdef ACHERON_HAVE_FFMPEG
    return d->audio ? d->audio->volume() : 0.0f;
#else
    return 0.0f;
#endif
}

void Player::setMuted(bool muted)
{
#ifdef ACHERON_HAVE_FFMPEG
    if (d->audio)
        d->audio->setMuted(muted);
#else
    Q_UNUSED(muted);
#endif
}

bool Player::isMuted() const
{
#ifdef ACHERON_HAVE_FFMPEG
    return d->audio && d->audio->isMuted();
#else
    return true;
#endif
}

void Player::setTargetSize(const QSize &size)
{
#ifdef ACHERON_HAVE_FFMPEG
    std::lock_guard<std::mutex> lock(d->mutex);
    d->targetSize = size;
#else
    Q_UNUSED(size);
#endif
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
#ifdef ACHERON_HAVE_FFMPEG
    return d->demuxedMs.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

QSize Player::nativeSize() const
{
    return d->native;
}

bool Player::hasAudio() const
{
    return d->audioAvailable;
}

bool Player::hasVideo() const
{
    return d->videoAvailable;
}

Player::State Player::state() const
{
    return d->state;
}

QString Player::errorString() const
{
    return d->error;
}

bool Player::isSeekable() const
{
    return d->seekable;
}

#ifdef ACHERON_HAVE_FFMPEG

void Player::Impl::setState(Player::State next)
{
    if (state == next)
        return;
    state = next;
    emit owner->stateChanged(next);
}

void Player::Impl::fail(const QString &message)
{
    error = message;
    qCWarning(LogVideo) << "playback failed:" << message;
    setState(Player::State::Error);
    emit owner->errorOccurred(message);
}

void Player::Impl::startWorker(const QString &input)
{
    stopWorker();

    if (!audio)
        audio = std::make_unique<AudioOutput>();

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

int Player::Impl::interruptCallback(void *opaque)
{
    auto *impl = static_cast<Player::Impl *>(opaque);
    return impl && impl->demuxInterrupted() ? 1 : 0;
}

bool Player::Impl::openInput(const QString &input)
{
    fmtCtx = avformat_alloc_context();
    if (!fmtCtx)
        return false;

    fmtCtx->interrupt_callback.callback = &Player::Impl::interruptCallback;
    fmtCtx->interrupt_callback.opaque = this;

    AVDictionary *options = nullptr;
    const QByteArray userAgent = Discord::CurlUtils::getUserAgent().toUtf8();
    av_dict_set(&options, "user_agent", userAgent.constData(), 0);
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);
    av_dict_set(&options, "rw_timeout", "15000000", 0);

    const QByteArray inputUtf8 = input.toUtf8();
    int ret = avformat_open_input(&fmtCtx, inputUtf8.constData(), nullptr, &options);
    av_dict_free(&options);

    if (ret < 0) {
        postToOwner([this, ret] { fail(averrorString(ret)); });
        return false;
    }

    if ((ret = avformat_find_stream_info(fmtCtx, nullptr)) < 0) {
        postToOwner([this, ret] { fail(averrorString(ret)); });
        return false;
    }

    videoStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    auto openCodec = [this](int streamIndex, AVCodecContext **out) -> bool {
        if (streamIndex < 0)
            return false;

        AVStream *stream = fmtCtx->streams[streamIndex];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec)
            return false;

        AVCodecContext *ctx = avcodec_alloc_context3(codec);
        if (!ctx)
            return false;

        if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        ctx->thread_count = qBound(1, cores > 0 ? cores : 2, MaxDecoderThreads);
        if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
            ctx->thread_type = FF_THREAD_FRAME;
        else if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
            ctx->thread_type = FF_THREAD_SLICE;

        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        *out = ctx;
        return true;
    };

    if (!openCodec(videoStream, &videoCtx))
        videoStream = -1;
    if (!openCodec(audioStream, &audioCtx))
        audioStream = -1;

    if (videoStream < 0 && audioStream < 0) {
        postToOwner([this] { fail(Player::tr("no playable stream found")); });
        return false;
    }

    if (videoStream < 0)
        primed.store(true, std::memory_order_relaxed);

    if (audioCtx) {
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, AudioOutput::Channels);

        ret = swr_alloc_set_opts2(&swrCtx,
                                  &outLayout,
                                  AV_SAMPLE_FMT_FLT,
                                  AudioOutput::SampleRate,
                                  &audioCtx->ch_layout,
                                  audioCtx->sample_fmt,
                                  audioCtx->sample_rate,
                                  0,
                                  nullptr);
        av_channel_layout_uninit(&outLayout);

        if (ret < 0 || swr_init(swrCtx) < 0) {
            swr_free(&swrCtx);
            avcodec_free_context(&audioCtx);
            audioStream = -1;
        }
    }

    const QSize nativeSize = videoCtx ? QSize(videoCtx->width, videoCtx->height) : QSize();
    const qint64 duration = fmtCtx->duration != AV_NOPTS_VALUE
                                    ? fmtCtx->duration * 1000 / AV_TIME_BASE
                                    : 0;
    const bool canSeek = duration > 0 && (!fmtCtx->pb || fmtCtx->pb->seekable != 0);
    const bool haveAudio = audioStream >= 0;
    const bool haveVideo = videoStream >= 0;

    postToOwner([this, nativeSize, duration, canSeek, haveAudio, haveVideo] {
        native = nativeSize;
        durationMs = duration;
        seekable = canSeek;
        audioAvailable = haveAudio;
        videoAvailable = haveVideo;
        emit owner->durationChanged(duration);
    });

    return true;
}

void Player::Impl::teardownInput()
{
    if (swsCtx) {
        sws_freeContext(swsCtx);
        swsCtx = nullptr;
    }
    if (swrCtx)
        swr_free(&swrCtx);
    if (videoCtx)
        avcodec_free_context(&videoCtx);
    if (audioCtx)
        avcodec_free_context(&audioCtx);
    if (fmtCtx)
        avformat_close_input(&fmtCtx);

    videoStream = -1;
    audioStream = -1;
    swsTarget = QSize();
    {
        std::lock_guard<std::mutex> lock(mutex);
        clearPacketQueueLocked();
    }
    clearPendingAudio();
}

void Player::Impl::clearPacketQueueLocked()
{
    for (QueuedPacket &queued : packetQueue) {
        if (queued.packet)
            av_packet_free(&queued.packet);
    }
    packetQueue.clear();
    packetQueueBytes = 0;
}

void Player::Impl::decodePacket(AVPacket *pkt, AVFrame *avFrame)
{
    AVCodecContext *ctx = nullptr;
    if (pkt->stream_index == videoStream)
        ctx = videoCtx;
    else if (pkt->stream_index == audioStream)
        ctx = audioCtx;

    if (!ctx || avcodec_send_packet(ctx, pkt) < 0)
        return;

    while (avcodec_receive_frame(ctx, avFrame) >= 0) {
        if (ctx == videoCtx)
            pushVideoFrame(avFrame);
        else
            pushAudioFrame(avFrame);
        av_frame_unref(avFrame);
    }
}

void Player::Impl::demuxSeek(qint64 targetMs)
{
    const int64_t ts = targetMs * AV_TIME_BASE / 1000;
    const int ret = av_seek_frame(fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
        qCWarning(LogVideo) << "seek failed:" << averrorString(ret);

    demuxEof.store(false, std::memory_order_relaxed);
    demuxedMs.store(targetMs, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mutex);
        clearPacketQueueLocked();
        packetQueue.push_back({ nullptr, targetMs });
        flushPending.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();
}

void Player::Impl::applyFlush(qint64 targetMs)
{
    if (videoCtx)
        avcodec_flush_buffers(videoCtx);
    if (audioCtx)
        avcodec_flush_buffers(audioCtx);

    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
    }
    clearPendingAudio();

    if (audio)
        audio->reset(targetMs);

    reachedEnd.store(false, std::memory_order_relaxed);
    seekTargetMs.store(targetMs, std::memory_order_relaxed);

    if (videoStream >= 0) {
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
            if (packetQueueBytes >= MaxBufferedBytes ||
                demuxEof.load(std::memory_order_relaxed)) {
                cv.wait(lock, [this] {
                    return demuxInterrupted() ||
                           (!demuxEof.load(std::memory_order_relaxed) &&
                            packetQueueBytes < MaxBufferedBytes);
                });
                continue;
            }
        }

        const int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            if (demuxInterrupted())
                continue;

            if (ret == AVERROR(EAGAIN)) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }

            if (ret != AVERROR_EOF)
                qCWarning(LogVideo) << "read failed:" << averrorString(ret);

            if (fmtCtx->duration != AV_NOPTS_VALUE)
                demuxedMs.store(fmtCtx->duration * 1000 / AV_TIME_BASE, std::memory_order_relaxed);
            demuxEof.store(true, std::memory_order_relaxed);
            cv.notify_all();
            continue;
        }

        if (packet->stream_index != videoStream && packet->stream_index != audioStream) {
            av_packet_unref(packet);
            continue;
        }

        const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (ts != AV_NOPTS_VALUE) {
            const AVStream *stream = fmtCtx->streams[packet->stream_index];
            const qint64 tsMs = static_cast<qint64>(ts * av_q2d(stream->time_base) * 1000.0);
            if (tsMs > demuxedMs.load(std::memory_order_relaxed))
                demuxedMs.store(tsMs, std::memory_order_relaxed);
        }

        AVPacket *stored = av_packet_alloc();
        av_packet_move_ref(stored, packet);

        bool wasEmpty;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wasEmpty = packetQueue.empty();
            packetQueueBytes += static_cast<size_t>(stored->size);
            packetQueue.push_back({ stored, -1 });
        }

        if (wasEmpty)
            cv.notify_all();
    }

    av_packet_free(&packet);
}

qint64 Player::Impl::framePtsMs(const AVFrame *avFrame, int streamIndex) const
{
    if (!fmtCtx || streamIndex < 0)
        return 0;

    const int64_t pts = avFrame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? avFrame->best_effort_timestamp
                                : avFrame->pts;
    if (pts == AV_NOPTS_VALUE)
        return 0;

    return static_cast<qint64>(pts * av_q2d(fmtCtx->streams[streamIndex]->time_base) * 1000.0);
}

bool Player::Impl::pushVideoFrame(AVFrame *avFrame)
{
    const qint64 ptsMs = framePtsMs(avFrame, videoStream);

    if (beforeSeekTarget(ptsMs))
        return false;

    QSize want;
    {
        std::lock_guard<std::mutex> lock(mutex);
        want = targetSize;
    }

    const QSize source(avFrame->width, avFrame->height);
    const QSize target = effectiveTarget(source, want);
    if (!target.isValid() || target.isEmpty())
        return false;

    if (!swsCtx || swsTarget != target) {
        if (swsCtx)
            sws_freeContext(swsCtx);

        swsCtx = sws_getContext(avFrame->width, avFrame->height,
                                static_cast<AVPixelFormat>(avFrame->format),
                                target.width(), target.height(),
                                AV_PIX_FMT_RGB32,
                                SWS_BILINEAR,
                                nullptr, nullptr, nullptr);
        if (!swsCtx)
            return false;
        swsTarget = target;
    }

    QImage image(target.width(), target.height(), QImage::Format_RGB32);
    uint8_t *dstData[4] = { image.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };
    sws_scale(swsCtx, avFrame->data, avFrame->linesize, 0, avFrame->height, dstData, dstStride);

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
            if (presentTimer && !presentTimer->isActive())
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
    if (!swrCtx || !audio || audioSinkBroken.load(std::memory_order_relaxed))
        return;

    if (beforeSeekTarget(framePtsMs(avFrame, audioStream)))
        return;

    const int maxOut = static_cast<int>(av_rescale_rnd(swr_get_delay(swrCtx, audioCtx->sample_rate) + avFrame->nb_samples,
                                                       AudioOutput::SampleRate,
                                                       audioCtx->sample_rate,
                                                       AV_ROUND_UP));

    audioScratch.resize(static_cast<size_t>(maxOut) * AudioOutput::Channels);

    uint8_t *out = reinterpret_cast<uint8_t *>(audioScratch.data());
    const int converted = swr_convert(swrCtx,
                                      &out,
                                      maxOut,
                                      const_cast<const uint8_t **>(avFrame->data),
                                      avFrame->nb_samples);
    if (converted <= 0)
        return;

    audioPending.insert(audioPending.end(),
                        audioScratch.begin(),
                        audioScratch.begin() + static_cast<size_t>(converted) * AudioOutput::Channels);
    drainPendingAudio();
}

void Player::Impl::drainPendingAudio()
{
    if (audioPendingEmpty() || !audio)
        return;

    if (audioSinkBroken.load(std::memory_order_relaxed)) {
        clearPendingAudio();
        return;
    }

    const size_t available = audioPending.size() - audioPendingOffset;
    const int pendingFrames = static_cast<int>(available / AudioOutput::Channels);
    const int written = audio->write(audioPending.data() + audioPendingOffset, pendingFrames);
    if (written <= 0)
        return;

    audioPendingOffset += static_cast<size_t>(written) * AudioOutput::Channels;
    if (audioPendingEmpty())
        clearPendingAudio();
}

void Player::Impl::decodeLoop(QString input)
{
    if (!openInput(input)) {
        teardownInput();
        return;
    }

    postToOwner([this] {
        if (state != Player::State::Opening)
            return;
        setState(Player::State::Paused);
        if (autoPlayOnReady) {
            autoPlayOnReady = false;
            owner->play();
        }
    });

    demuxer = std::thread(&Player::Impl::demuxLoop, this);

    AVFrame *avFrame = av_frame_alloc();

    while (running.load(std::memory_order_relaxed)) {
        drainPendingAudio();

        const bool decodable = (!paused.load(std::memory_order_relaxed) &&
                                !reachedEnd.load(std::memory_order_relaxed)) ||
                               !primed.load(std::memory_order_relaxed);

        AVPacket *next = nullptr;
        qint64 flushTarget = -1;
        bool starved = false;
        bool queueWasFull = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (packetQueue.empty()) {
                starved = true;
            } else if (!packetQueue.front().packet) {
                flushTarget = packetQueue.front().flushToMs;
                packetQueue.pop_front();
                flushPending.store(false, std::memory_order_relaxed);
            } else if (decodable) {
                const bool videoRoom = videoStream >= 0 && frames.size() < MaxQueuedFrames;
                const bool audioRoom = audioSinkUsable() &&
                                       audio->writableFrames() >= AudioChunkFrames &&
                                       audioPendingEmpty();
                if (videoRoom || audioRoom) {
                    queueWasFull = packetQueueBytes >= MaxBufferedBytes;
                    next = packetQueue.front().packet;
                    packetQueue.pop_front();
                    packetQueueBytes -= static_cast<size_t>(next->size);
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
        if (decodable && !packetQueue.empty() && audioSinkUsable()) {
            const qint64 waitMs = qBound(qint64(5),
                                         qint64(audio->bufferedFrames()) * 1000 / AudioOutput::SampleRate / 2,
                                         qint64(250));
            cv.wait_for(lock, std::chrono::milliseconds(waitMs));
        } else {
            cv.wait(lock, [this] {
                if (!running.load(std::memory_order_relaxed))
                    return true;

                if (packetQueue.empty())
                    return demuxEof.load(std::memory_order_relaxed) &&
                           !reachedEnd.load(std::memory_order_relaxed) &&
                           !seekInFlight();

                if (!packetQueue.front().packet)
                    return true;

                const bool mayDecode = (!paused.load(std::memory_order_relaxed) &&
                                        !reachedEnd.load(std::memory_order_relaxed)) ||
                                       !primed.load(std::memory_order_relaxed);
                return mayDecode && (videoStream < 0 || frames.size() < MaxQueuedFrames);
            });
        }
    }

    av_frame_free(&avFrame);

    if (demuxer.joinable()) {
        cv.notify_all();
        demuxer.join();
    }

    teardownInput();
}

void Player::Impl::drainAtEnd(AVFrame *avFrame)
{
    if (videoCtx && avcodec_send_packet(videoCtx, nullptr) >= 0) {
        while (!interrupted()) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] {
                    return interrupted() || frames.size() < MaxQueuedFrames;
                });
            }
            if (interrupted() || avcodec_receive_frame(videoCtx, avFrame) < 0)
                break;

            pushVideoFrame(avFrame);
            av_frame_unref(avFrame);
        }
    }

    if (audioCtx && avcodec_send_packet(audioCtx, nullptr) >= 0) {
        while (!interrupted() && avcodec_receive_frame(audioCtx, avFrame) >= 0) {
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

        bool audioDrained = !audioSinkUsable();
        if (!audioDrained && audioPendingEmpty()) {
            const int buffered = audio ? audio->bufferedFrames() : 0;

            audioDrained = buffered == 0 ||
                           (audio && !audio->isRunning() &&
                            !paused.load(std::memory_order_relaxed));
        }

        if (audioDrained && !clockHandedOver && audioSinkUsable()) {
            clockHandedOver = true;
            postToOwner([this] {
                audioClockStalled = true;
                if (state == Player::State::Playing) {
                    wallBaseMs = audio ? audio->clockMs() : positionMs;
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
            if (presentTimer)
                presentTimer->stop();
            if (audio)
                audio->stop();
        }
    });
}

qint64 Player::Impl::clockMs() const
{
    if (audio && audioAvailable && !audioClockStalled)
        return audio->clockMs();
    if (paused.load(std::memory_order_relaxed))
        return wallBaseMs;
    return wallBaseMs + wallTimer.elapsed();
}

void Player::Impl::scheduleNextPresent(qint64 clock)
{
    if (!presentTimer || state != Player::State::Playing)
        return;

    qint64 delay = 16;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!frames.empty())
            delay = frames.front().ptsMs - clock;
    }

    presentTimer->start(static_cast<int>(qBound(MinPresentDelayMs, delay, MaxPresentDelayMs)));
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

} // namespace Video
} // namespace Core
} // namespace Acheron
