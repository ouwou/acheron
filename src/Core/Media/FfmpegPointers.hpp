#pragma once

#include <QSize>
#include <QString>

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace Acheron {
namespace Core {
namespace Media {

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter
{
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
using PacketPointer = std::unique_ptr<AVPacket, PacketDeleter>;

struct CodecContextDeleter
{
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};
using CodecContextPointer = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct InputCloser
{
    void operator()(AVFormatContext *input) const { avformat_close_input(&input); }
};
using InputPointer = std::unique_ptr<AVFormatContext, InputCloser>;

// keeps the frame's own buffer when it already holds a writable picture of this format and size
[[nodiscard]] inline int prepareWritable(AVFrame *frame, int format, const QSize &size)
{
    if (frame->buf[0] && frame->format == format && frame->width == size.width() && frame->height == size.height() && av_frame_make_writable(frame) >= 0)
        return 0;
    av_frame_unref(frame);
    frame->format = format;
    frame->width = size.width();
    frame->height = size.height();
    return av_frame_get_buffer(frame, 0);
}

[[nodiscard]] inline QString describeError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return QString::fromUtf8(text);
}

} // namespace Media
} // namespace Core
} // namespace Acheron
