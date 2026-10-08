#include "Core/Media/ClipDecoder.hpp"

#include "Core/AnimationTiming.hpp"

#include <QFile>
#include <QImageReader>
#include <QPainter>

#include <algorithm>
#include <optional>

#ifdef ACHERON_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#endif // ACHERON_HAVE_FFMPEG

namespace Acheron {
namespace Core {
namespace Media {

namespace {

enum class Container {
    Unknown,
    Gif,
    Mp4,
    WebM,
    WebP,
};

constexpr int SniffedBytes = 12;

Container sniffContainer(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Container::Unknown;

    const QByteArray head = file.read(SniffedBytes);
    if (head.startsWith("GIF8"))
        return Container::Gif;
    if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return Container::WebP;
    if (head.mid(4, 4) == "ftyp")
        return Container::Mp4;
    if (head.startsWith("\x1a\x45\xdf\xa3"))
        return Container::WebM;
    return Container::Unknown;
}

void premultiplyInto(QImage &reused, const QImage &source)
{
    const bool reusable = reused.size() == source.size() && reused.format() == QImage::Format_ARGB32_Premultiplied && reused.isDetached();
    if (!reusable)
        reused = QImage(source.size(), QImage::Format_ARGB32_Premultiplied);

    QPainter painter(&reused);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(0, 0, source);
}

class ImageReaderClipDecoder final : public ClipDecoder
{
public:
    static std::unique_ptr<ClipDecoder> open(const QString &path)
    {
        auto decoder = std::unique_ptr<ImageReaderClipDecoder>(new ImageReaderClipDecoder(path));
        if (!decoder->file.open(QIODevice::ReadOnly) || !decoder->rewind() || !decoder->reader->supportsAnimation())
            return nullptr;
        return decoder;
    }

    QSize nativeSize() const override { return reader->size(); }

    bool nextFrameOfLoop(QImage &frame, qint64 &startMs, const QSize &frameSize) override
    {
        QImage image = reader->read();
        if (image.isNull())
            return false;

        startMs = elapsedMs;
        elapsedMs += normalizedFrameDelayMs(reader->nextImageDelay());

        if (image.size() != frameSize)
            image = image.scaled(frameSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        premultiplyInto(frame, image);
        return !frame.isNull();
    }

    qint64 finishedLoopMs() const override { return elapsedMs; }

    bool rewind() override
    {
        elapsedMs = 0;
        file.seek(0);
        reader = std::make_unique<QImageReader>(&file);
        return reader->canRead();
    }

private:
    explicit ImageReaderClipDecoder(const QString &path) : file(path) {}

    QFile file;
    std::unique_ptr<QImageReader> reader;
    qint64 elapsedMs = 0;
};

#ifdef ACHERON_HAVE_FFMPEG

constexpr int SwscaleOverrunBytes = 64;
constexpr qint64 UnknownLastFrameMs = 100;
constexpr int FramesReadAheadToGuessFrameRate = 0;

QImage imageWithSwscaleOverrunRoom(const QSize &size, QImage::Format format)
{
    const int stride = size.width() * 4;
    auto *pixels = static_cast<uchar *>(av_malloc(size_t(stride) * size_t(size.height()) + SwscaleOverrunBytes));
    if (!pixels)
        return {};
    return QImage(pixels, size.width(), size.height(), stride, format, [](void *owned) { av_free(owned); }, pixels);
}

void premultiply(QImage &straightAlpha)
{
    auto *pixel = reinterpret_cast<QRgb *>(straightAlpha.bits());
    for (const QRgb *end = pixel + qsizetype(straightAlpha.width()) * straightAlpha.height(); pixel != end; ++pixel) {
        if (qAlpha(*pixel) != 255)
            *pixel = qPremultiply(*pixel);
    }
}

class FfmpegClipDecoder final : public ClipDecoder
{
public:
    static std::unique_ptr<ClipDecoder> open(const QString &path, const char *demuxer)
    {
        auto decoder = std::unique_ptr<FfmpegClipDecoder>(new FfmpegClipDecoder(path, demuxer));
        if (!decoder->decoded || !decoder->packet || !decoder->openInput())
            return nullptr;
        return decoder;
    }

    ~FfmpegClipDecoder() override
    {
        closeInput();
        sws_freeContext(scaler);
        av_frame_free(&decoded);
        av_packet_free(&packet);
    }

    QSize nativeSize() const override { return QSize(codec->width, codec->height); }

    bool nextFrameOfLoop(QImage &frame, qint64 &startMs, const QSize &frameSize) override
    {
        if (!receiveFrame())
            return false;

        const int64_t pts = decoded->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE) {
            if (!firstPts)
                firstPts = pts;
            startMs = std::max(lastStartMs, toMs(pts - *firstPts));
        } else {
            startMs = lastStartMs;
        }
        lastGapMs = startMs - lastStartMs;
        lastStartMs = startMs;

        const bool converted = convert(frame, frameSize);
        av_frame_unref(decoded);
        return converted;
    }

    qint64 finishedLoopMs() const override
    {
        const qint64 demuxedMs = firstPts && streamEndPts ? toMs(*streamEndPts - *firstPts) : 0;
        if (demuxedMs > lastStartMs)
            return demuxedMs;
        return lastStartMs + (lastGapMs > 0 ? lastGapMs : UnknownLastFrameMs);
    }

    bool rewind() override
    {
        draining = false;
        lastStartMs = 0;
        lastGapMs = 0;

        avcodec_flush_buffers(codec);
        if (av_seek_frame(format, videoStream, firstPts.value_or(0), AVSEEK_FLAG_BACKWARD) >= 0)
            return true;

        closeInput();
        return openInput();
    }

private:
    FfmpegClipDecoder(const QString &path, const char *demuxer)
        : path(path.toUtf8()), demuxer(demuxer), decoded(av_frame_alloc()), packet(av_packet_alloc())
    {
    }

    bool openInput()
    {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        const int opened = avformat_open_input(&format, path.constData(), av_find_input_format(demuxer), &options);
        av_dict_free(&options);
        if (opened < 0)
            return false;

        format->fps_probe_size = FramesReadAheadToGuessFrameRate;
        if (avformat_find_stream_info(format, nullptr) < 0)
            return false;

        videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0)
            return false;

        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (int(i) != videoStream)
                format->streams[i]->discard = AVDISCARD_ALL;
        }

        const AVStream *stream = format->streams[videoStream];
        timeBase = stream->time_base;

        const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder)
            return false;

        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0)
            return false;

        codec->thread_count = 1;
        return avcodec_open2(codec, decoder, nullptr) >= 0 && codec->width > 0 && codec->height > 0;
    }

    void closeInput()
    {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        videoStream = -1;
    }

    [[nodiscard]] qint64 toMs(int64_t streamTicks) const { return av_rescale_q(streamTicks, timeBase, AVRational{ 1, 1000 }); }

    void feedDecoder()
    {
        for (;;) {
            if (av_read_frame(format, packet) < 0) {
                avcodec_send_packet(codec, nullptr);
                draining = true;
                return;
            }

            const bool wanted = packet->stream_index == videoStream;
            if (wanted && packet->pts != AV_NOPTS_VALUE)
                streamEndPts = std::max(streamEndPts.value_or(packet->pts), packet->pts + std::max<int64_t>(packet->duration, 0));

            const int sent = wanted ? avcodec_send_packet(codec, packet) : AVERROR(EAGAIN);
            av_packet_unref(packet);
            if (sent >= 0)
                return;
        }
    }

    bool receiveFrame()
    {
        for (;;) {
            const int received = avcodec_receive_frame(codec, decoded);
            if (received == 0)
                return true;
            if (received != AVERROR(EAGAIN) || draining)
                return false;
            feedDecoder();
        }
    }

    bool convert(QImage &frame, const QSize &frameSize)
    {
        const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(AVPixelFormat(decoded->format));
        const bool hasAlpha = layout && (layout->flags & AV_PIX_FMT_FLAG_ALPHA);
        const QImage::Format wanted = hasAlpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;

        const bool reusable = frame.size() == frameSize && frame.format() == wanted && frame.isDetached();
        if (!reusable)
            frame = imageWithSwscaleOverrunRoom(frameSize, wanted);
        if (frame.isNull())
            return false;

        scaler = sws_getCachedContext(scaler, decoded->width, decoded->height, AVPixelFormat(decoded->format), frameSize.width(),
                                      frameSize.height(), AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler)
            return false;

        uint8_t *planes[4] = { frame.bits(), nullptr, nullptr, nullptr };
        int strides[4] = { int(frame.bytesPerLine()), 0, 0, 0 };
        sws_scale(scaler, decoded->data, decoded->linesize, 0, decoded->height, planes, strides);

        if (hasAlpha)
            premultiply(frame);
        return true;
    }

    QByteArray path;
    const char *demuxer;

    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    SwsContext *scaler = nullptr;
    AVFrame *decoded;
    AVPacket *packet;
    int videoStream = -1;
    AVRational timeBase{ 1, 1000 };

    std::optional<int64_t> firstPts;
    std::optional<int64_t> streamEndPts;
    qint64 lastStartMs = 0;
    qint64 lastGapMs = 0;
    bool draining = false;
};

std::unique_ptr<ClipDecoder> openWithFfmpeg(const QString &path, Container container)
{
    switch (container) {
    case Container::Gif:
        return FfmpegClipDecoder::open(path, "gif");
    case Container::Mp4:
        return FfmpegClipDecoder::open(path, "mov");
    case Container::WebM:
        return FfmpegClipDecoder::open(path, "matroska");
    case Container::WebP:
    case Container::Unknown:
        break;
    }
    return nullptr;
}

#else // ACHERON_HAVE_FFMPEG

std::unique_ptr<ClipDecoder> openWithFfmpeg(const QString &, Container)
{
    return nullptr;
}

#endif // ACHERON_HAVE_FFMPEG

} // namespace

std::unique_ptr<ClipDecoder> ClipDecoder::open(const QString &path)
{
    const Container container = sniffContainer(path);
    if (auto decoder = openWithFfmpeg(path, container))
        return decoder;

    const bool qtCanAnimate = container == Container::WebP || container == Container::Gif;
    return qtCanAnimate ? ImageReaderClipDecoder::open(path) : nullptr;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
