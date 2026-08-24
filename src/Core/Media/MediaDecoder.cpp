#include "Core/Media/MediaDecoder.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#include "Core/Logging.hpp"
#include "Core/Media/AudioOutput.hpp"
#include "Discord/CurlUtils.hpp"

#include <QCoreApplication>

#include <thread>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57, 28, 100)
#error "ffmpeg 5.1 or newer is required for video support"
#endif

namespace Acheron {
namespace Core {
namespace Media {

namespace {
constexpr int MaxDecoderThreads = 2;
} // namespace

MediaDecoder::~MediaDecoder()
{
    close();
}

QString MediaDecoder::errorString(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

void MediaDecoder::setInterrupt(int (*callback)(void *), void *opaque)
{
    interruptCallback = callback;
    interruptOpaque = opaque;
}

bool MediaDecoder::open(const QString &input, QString *error)
{
    fmtCtx = avformat_alloc_context();
    if (!fmtCtx) {
        if (error)
            *error = QCoreApplication::translate("Acheron::Core::Media::Player", "out of memory");
        return false;
    }

    if (interruptCallback) {
        fmtCtx->interrupt_callback.callback = interruptCallback;
        fmtCtx->interrupt_callback.opaque = interruptOpaque;
    }

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
        if (error)
            *error = errorString(ret);
        return false;
    }

    if ((ret = avformat_find_stream_info(fmtCtx, nullptr)) < 0) {
        if (error)
            *error = errorString(ret);
        return false;
    }

    videoStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStream = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (!openCodec(videoStream, &videoCtx))
        videoStream = -1;
    if (!openCodec(audioStream, &audioCtx))
        audioStream = -1;

    if (videoStream < 0 && audioStream < 0) {
        if (error)
            *error = QCoreApplication::translate("Acheron::Core::Media::Player", "no playable stream found");
        return false;
    }

    if (audioCtx && !openResampler()) {
        swr_free(&swrCtx);
        avcodec_free_context(&audioCtx);
        audioStream = -1;
    }

    return true;
}

bool MediaDecoder::openCodec(int streamIndex, AVCodecContext **out)
{
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
}

bool MediaDecoder::openResampler()
{
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, AudioOutput::Channels);

    const int ret = swr_alloc_set_opts2(&swrCtx,
                                        &outLayout,
                                        AV_SAMPLE_FMT_FLT,
                                        AudioOutput::SampleRate,
                                        &audioCtx->ch_layout,
                                        audioCtx->sample_fmt,
                                        audioCtx->sample_rate,
                                        0,
                                        nullptr);
    av_channel_layout_uninit(&outLayout);

    return ret >= 0 && swr_init(swrCtx) >= 0;
}

void MediaDecoder::close()
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
}

MediaDecoder::Info MediaDecoder::info() const
{
    Info result;
    if (!fmtCtx)
        return result;

    result.native = videoCtx ? QSize(videoCtx->width, videoCtx->height) : QSize();
    result.durationMs = containerDurationMs();
    result.seekable = result.durationMs > 0 && (!fmtCtx->pb || fmtCtx->pb->seekable != 0);
    result.hasAudio = audioStream >= 0;
    result.hasVideo = videoStream >= 0;
    return result;
}

qint64 MediaDecoder::containerDurationMs() const
{
    if (!fmtCtx || fmtCtx->duration == AV_NOPTS_VALUE)
        return 0;
    return fmtCtx->duration * 1000 / AV_TIME_BASE;
}

int MediaDecoder::readPacket(AVPacket *packet)
{
    return av_read_frame(fmtCtx, packet);
}

MediaDecoder::Stream MediaDecoder::streamOf(const AVPacket *packet) const
{
    if (packet->stream_index == videoStream)
        return Stream::Video;
    if (packet->stream_index == audioStream)
        return Stream::Audio;
    return Stream::None;
}

int MediaDecoder::streamIndexFor(Stream stream) const
{
    switch (stream) {
    case Stream::Video:
        return videoStream;
    case Stream::Audio:
        return audioStream;
    case Stream::None:
        break;
    }
    return -1;
}

AVCodecContext *MediaDecoder::contextFor(Stream stream) const
{
    switch (stream) {
    case Stream::Video:
        return videoCtx;
    case Stream::Audio:
        return audioCtx;
    case Stream::None:
        break;
    }
    return nullptr;
}

qint64 MediaDecoder::packetTimestampMs(const AVPacket *packet) const
{
    const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (ts == AV_NOPTS_VALUE || !fmtCtx)
        return -1;

    const AVStream *stream = fmtCtx->streams[packet->stream_index];
    return static_cast<qint64>(ts * av_q2d(stream->time_base) * 1000.0);
}

qint64 MediaDecoder::frameTimestampMs(const AVFrame *frame, Stream stream) const
{
    const int streamIndex = streamIndexFor(stream);
    if (!fmtCtx || streamIndex < 0)
        return 0;

    const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp
                                                                       : frame->pts;
    if (pts == AV_NOPTS_VALUE)
        return 0;

    return static_cast<qint64>(pts * av_q2d(fmtCtx->streams[streamIndex]->time_base) * 1000.0);
}

bool MediaDecoder::seek(qint64 targetMs, QString *error)
{
    const int64_t ts = targetMs * AV_TIME_BASE / 1000;
    const int ret = av_seek_frame(fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0 && error)
        *error = errorString(ret);
    return ret >= 0;
}

void MediaDecoder::flush()
{
    if (videoCtx)
        avcodec_flush_buffers(videoCtx);
    if (audioCtx)
        avcodec_flush_buffers(audioCtx);
}

bool MediaDecoder::sendPacket(Stream stream, AVPacket *packet)
{
    AVCodecContext *ctx = contextFor(stream);
    return ctx && avcodec_send_packet(ctx, packet) >= 0;
}

int MediaDecoder::receiveFrame(Stream stream, AVFrame *frame)
{
    AVCodecContext *ctx = contextFor(stream);
    return ctx ? avcodec_receive_frame(ctx, frame) : AVERROR(EINVAL);
}

QImage MediaDecoder::scaleToImage(const AVFrame *frame, const QSize &target)
{
    if (target.isEmpty())
        return QImage();

    if (!swsCtx || swsTarget != target) {
        if (swsCtx)
            sws_freeContext(swsCtx);

        swsCtx = sws_getContext(frame->width, frame->height,
                                static_cast<AVPixelFormat>(frame->format),
                                target.width(), target.height(),
                                AV_PIX_FMT_RGB32,
                                SWS_BILINEAR,
                                nullptr, nullptr, nullptr);
        if (!swsCtx)
            return QImage();
        swsTarget = target;
    }

    // swscale will oob without extra room because of simd stuff
    const int stride = target.width() * 4;
    const int spareRows = (64 + stride - 1) / stride;

    auto *storage = new QImage(target.width(), target.height() + spareRows, QImage::Format_RGB32);
    if (storage->isNull()) {
        delete storage;
        return QImage();
    }

    uint8_t *dstData[4] = { storage->bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(storage->bytesPerLine()), 0, 0, 0 };
    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstStride);

    return QImage(storage->bits(), target.width(), target.height(), storage->bytesPerLine(), QImage::Format_RGB32, [](void *owner) { delete static_cast<QImage *>(owner); }, storage);
}

int MediaDecoder::appendResampledFrames(const AVFrame *frame, std::vector<float> &out,
                                        std::vector<float> &scratch)
{
    if (!swrCtx || !audioCtx)
        return 0;

    const int maxOut = static_cast<int>(av_rescale_rnd(swr_get_delay(swrCtx, audioCtx->sample_rate) + frame->nb_samples,
                                                       AudioOutput::SampleRate,
                                                       audioCtx->sample_rate,
                                                       AV_ROUND_UP));

    scratch.resize(static_cast<size_t>(maxOut) * AudioOutput::Channels);

    uint8_t *dst = reinterpret_cast<uint8_t *>(scratch.data());
    const int converted = swr_convert(swrCtx,
                                      &dst,
                                      maxOut,
                                      const_cast<const uint8_t **>(frame->data),
                                      frame->nb_samples);
    if (converted <= 0)
        return 0;

    out.insert(out.end(),
               scratch.begin(),
               scratch.begin() + static_cast<size_t>(converted) * AudioOutput::Channels);
    return converted;
}

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
