#include "ApngDecoder.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#endif

namespace Acheron {
namespace Core {
namespace Apng {

#ifdef ACHERON_HAVE_FFMPEG

namespace {

constexpr char Signature[] = "\x89PNG\r\n\x1a\n";
constexpr int SignatureSize = 8;
constexpr int IoBufferSize = 32 * 1024;
constexpr size_t MaxPackets = 1000;

#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
constexpr AVPixelFormat QImageArgb32Layout = AV_PIX_FMT_BGRA;
#else
constexpr AVPixelFormat QImageArgb32Layout = AV_PIX_FMT_ARGB;
#endif

struct MemorySource
{
    QByteArray data;
    qint64 pos = 0;
};

int readPacket(void *opaque, uint8_t *buffer, int size)
{
    auto *source = static_cast<MemorySource *>(opaque);
    const qint64 available = source->data.size() - source->pos;
    if (available <= 0)
        return AVERROR_EOF;

    const int count = int(std::min<qint64>(size, available));
    std::memcpy(buffer, source->data.constData() + source->pos, size_t(count));
    source->pos += count;
    return count;
}

int64_t seekPacket(void *opaque, int64_t offset, int whence)
{
    auto *source = static_cast<MemorySource *>(opaque);
    if (whence & AVSEEK_SIZE)
        return source->data.size();

    qint64 target = offset;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        target += source->pos;
        break;
    case SEEK_END:
        target += source->data.size();
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (target < 0 || target > source->data.size())
        return AVERROR(EINVAL);
    source->pos = target;
    return target;
}

} // namespace

struct Reader::Impl
{
    ~Impl()
    {
        for (AVPacket *packet : packets)
            av_packet_free(&packet);
        av_frame_free(&frame);
        sws_freeContext(scaler);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    bool openInput();
    bool readAllPackets();
    bool openCodec();
    bool receiveFrame();
    QImage frameAsImage();

    MemorySource source;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    SwsContext *scaler = nullptr;
    AVFrame *frame = nullptr;
    AVRational timeBase{ 1, 1000 };
    std::vector<AVPacket *> packets;
    size_t nextPacket = 0;
    size_t nextFrame = 0;
    bool flushing = false;
};

bool Reader::Impl::openInput()
{
    auto *buffer = static_cast<unsigned char *>(av_malloc(IoBufferSize));
    if (!buffer)
        return false;

    io = avio_alloc_context(buffer, IoBufferSize, 0, &source, readPacket, nullptr, seekPacket);
    if (!io) {
        av_free(buffer);
        return false;
    }

    format = avformat_alloc_context();
    if (!format)
        return false;
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&format, nullptr, av_find_input_format("apng"), nullptr) < 0)
        return false;

    if (format->nb_streams == 0 || format->streams[0]->codecpar->width <= 0 ||
        format->streams[0]->codecpar->height <= 0)
        return false;

    timeBase = format->streams[0]->time_base;
    return true;
}

bool Reader::Impl::readAllPackets()
{
    for (;;) {
        AVPacket *packet = av_packet_alloc();
        if (!packet)
            return false;
        if (av_read_frame(format, packet) < 0) {
            av_packet_free(&packet);
            break;
        }
        if (packet->stream_index != 0) {
            av_packet_free(&packet);
            continue;
        }
        packets.push_back(packet);
        if (packets.size() > MaxPackets)
            return false;
    }
    return packets.size() > 1;
}

bool Reader::Impl::openCodec()
{
    const AVCodecParameters *parameters = format->streams[0]->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(parameters->codec_id);
    if (!decoder)
        return false;

    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec, parameters) < 0)
        return false;

    if (avcodec_open2(codec, decoder, nullptr) < 0)
        return false;

    frame = av_frame_alloc();
    return frame != nullptr;
}

bool Reader::Impl::receiveFrame()
{
    for (;;) {
        const int received = avcodec_receive_frame(codec, frame);
        if (received == 0)
            return true;
        if (received != AVERROR(EAGAIN) || flushing)
            return false;

        if (nextPacket < packets.size()) {
            if (avcodec_send_packet(codec, packets[nextPacket++]) < 0)
                return false;
        } else {
            avcodec_send_packet(codec, nullptr);
            flushing = true;
        }
    }
}

QImage Reader::Impl::frameAsImage()
{
    scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                                  frame->height, QImageArgb32Layout, SWS_POINT, nullptr, nullptr, nullptr);
    if (!scaler)
        return {};

    QImage image(frame->width, frame->height, QImage::Format_ARGB32);
    uint8_t *planes[4] = { image.bits(), nullptr, nullptr, nullptr };
    int strides[4] = { int(image.bytesPerLine()), 0, 0, 0 };
    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
    return image;
}

std::unique_ptr<Reader> Reader::open(const QByteArray &png)
{
    if (!png.startsWith(QByteArray::fromRawData(Signature, SignatureSize)))
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->source.data = png;
    if (!impl->openInput() || !impl->readAllPackets() || !impl->openCodec())
        return nullptr;
    return std::unique_ptr<Reader>(new Reader(std::move(impl)));
}

QSize Reader::canvasSize() const
{
    const AVCodecParameters *parameters = impl->format->streams[0]->codecpar;
    return QSize(parameters->width, parameters->height);
}

int Reader::frameCount() const
{
    return int(impl->packets.size());
}

bool Reader::next(QImage &canvas, int &delayMs)
{
    if (impl->nextFrame >= impl->packets.size() || !impl->receiveFrame())
        return false;

    const int64_t duration = impl->packets[impl->nextFrame++]->duration;
    delayMs = int(av_rescale_q(std::max<int64_t>(duration, 0), impl->timeBase, AVRational{ 1, 1000 }));
    canvas = impl->frameAsImage();
    av_frame_unref(impl->frame);
    return !canvas.isNull();
}

#else

struct Reader::Impl
{
};

std::unique_ptr<Reader> Reader::open(const QByteArray &)
{
    return nullptr;
}

QSize Reader::canvasSize() const
{
    return {};
}

int Reader::frameCount() const
{
    return 0;
}

bool Reader::next(QImage &, int &)
{
    return false;
}

#endif

Reader::Reader(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

Reader::~Reader() = default;

} // namespace Apng
} // namespace Core
} // namespace Acheron
