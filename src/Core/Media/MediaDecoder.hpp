#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QImage>
#include <QSize>
#include <QString>

#include <vector>

#include "Core/ProxyConfig.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace Core {
namespace Media {

class MediaDecoder
{
public:
    enum class Stream {
        None,
        Video,
        Audio,
    };

    struct Info
    {
        QSize native;
        qint64 durationMs = 0;
        bool seekable = false;
        bool hasAudio = false;
        bool hasVideo = false;
    };

    MediaDecoder() = default;
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder &) = delete;
    MediaDecoder &operator=(const MediaDecoder &) = delete;

    void setInterrupt(int (*callback)(void *), void *opaque);

    bool open(const QString &input, const ProxyConfig &proxy, QString *error);
    void close();

    [[nodiscard]] Info info() const;
    [[nodiscard]] bool hasVideo() const { return videoStream >= 0; }
    [[nodiscard]] bool hasAudio() const { return audioStream >= 0; }
    [[nodiscard]] qint64 containerDurationMs() const;

    int readPacket(AVPacket *packet);
    [[nodiscard]] Stream streamOf(const AVPacket *packet) const;
    [[nodiscard]] qint64 packetTimestampMs(const AVPacket *packet) const;

    bool seek(qint64 targetMs, QString *error);
    void flush();

    bool sendPacket(Stream stream, AVPacket *packet);
    bool drain(Stream stream) { return sendPacket(stream, nullptr); }
    int receiveFrame(Stream stream, AVFrame *frame);
    [[nodiscard]] qint64 frameTimestampMs(const AVFrame *frame, Stream stream) const;

    QImage scaleToImage(const AVFrame *frame, const QSize &target);

    int appendResampledFrames(const AVFrame *frame, std::vector<float> &out, std::vector<float> &scratch);

    [[nodiscard]] static QString errorString(int code);

private:
    bool openCodec(int streamIndex, AVCodecContext **out);
    bool openResampler();
    [[nodiscard]] AVCodecContext *contextFor(Stream stream) const;
    [[nodiscard]] int streamIndexFor(Stream stream) const;

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *videoCtx = nullptr;
    AVCodecContext *audioCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    QSize swsTarget;

    static int denyNestedOpen(AVFormatContext *, AVIOContext **, const char *url, int, AVDictionary **);

    int (*interruptCallback)(void *) = nullptr;
    void *interruptOpaque = nullptr;

    AVIOContext *curlIo = nullptr;
};

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
