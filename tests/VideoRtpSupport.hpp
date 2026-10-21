#pragma once

// none of this uses the code under test: H264, H265 and VP8 go through ffmpeg's RTP muxer, and AV1 is
// packetized by hand from its RTP specification because the muxer doesnt handle it

#include "Discord/Voice/RtpPacket.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace VideoRtpSupport {

using Bytes = std::vector<uint8_t>;

constexpr int MTU = 1200;

inline void appendLeb128(Bytes &out, size_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

struct RtpPayloads
{
    std::vector<Bytes> payloads;
    std::vector<bool> markers;
};

// ffmpeg 7 made the buffer a write callback is handed const
#if LIBAVFORMAT_VERSION_MAJOR >= 61
using WrittenBytes = const uint8_t *;
#else
using WrittenBytes = uint8_t *;
#endif

inline int collectPacket(void *opaque, WrittenBytes data, int size)
{
    using namespace Acheron::Discord::Voice;

    auto *out = static_cast<RtpPayloads *>(opaque);
    const std::span<const uint8_t> packet(data, static_cast<size_t>(size));
    // the muxer interleaves its own sender reports
    if (classifyPacket(packet) != PacketKind::Rtp)
        return size;

    const std::optional<ParsedRtp> rtp = parseRtp(packet);
    if (rtp) {
        out->payloads.emplace_back(packet.begin() + rtp->clearSize + rtp->extensionSize, packet.end());
        out->markers.push_back(rtp->header.marker);
    }
    return size;
}

inline RtpPayloads packetizeWithFfmpeg(AVCodecID codec, const Bytes &frame, bool keyframe)
{
    RtpPayloads out;

    AVFormatContext *context = nullptr;
    if (avformat_alloc_output_context2(&context, nullptr, "rtp", nullptr) < 0)
        return out;

    AVStream *stream = avformat_new_stream(context, nullptr);
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = codec;
    stream->codecpar->width = 1280;
    stream->codecpar->height = 720;
    stream->time_base = { 1, 90000 };

    constexpr int IO_BUFFER_SIZE = 4096;
    auto *ioBuffer = static_cast<unsigned char *>(av_malloc(IO_BUFFER_SIZE));
    context->pb = avio_alloc_context(ioBuffer, IO_BUFFER_SIZE, 1, &out, nullptr, collectPacket, nullptr);
    context->pb->max_packet_size = MTU;

    if (avformat_write_header(context, nullptr) >= 0) {
        AVPacket *packet = av_packet_alloc();
        av_new_packet(packet, static_cast<int>(frame.size()));
        memcpy(packet->data, frame.data(), frame.size());
        packet->stream_index = 0;
        packet->pts = packet->dts = 3000;
        if (keyframe)
            packet->flags |= AV_PKT_FLAG_KEY;

        if (av_write_frame(context, packet) < 0)
            out = {};
        av_write_frame(context, nullptr);
        av_packet_free(&packet);
        av_write_trailer(context);
    }

    av_freep(&context->pb->buffer);
    avio_context_free(&context->pb);
    avformat_free_context(context);
    return out;
}

inline RtpPayloads packetizeAv1ByHand(const Bytes &frame, bool newSequence)
{
    constexpr uint8_t OBU_HAS_SIZE = 0x02;
    constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;

    std::vector<Bytes> elements;
    size_t offset = 0;
    while (offset < frame.size()) {
        const uint8_t header = frame[offset];
        size_t cursor = offset + 1;
        size_t size = frame.size() - cursor;
        if (header & OBU_HAS_SIZE) {
            size = 0;
            for (int shift = 0;; shift += 7) {
                const uint8_t byte = frame[cursor++];
                size |= size_t(byte & 0x7F) << shift;
                if (!(byte & 0x80))
                    break;
            }
        }
        if (((header >> 3) & 0x0F) != OBU_TEMPORAL_DELIMITER) {
            Bytes element;
            element.push_back(static_cast<uint8_t>(header & ~OBU_HAS_SIZE));
            element.insert(element.end(), frame.begin() + static_cast<std::ptrdiff_t>(cursor), frame.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            elements.push_back(std::move(element));
        }
        offset = cursor + size;
    }

    RtpPayloads out;
    Bytes packet = { static_cast<uint8_t>(newSequence ? 0x08 : 0x00) };
    for (const Bytes &element : elements) {
        size_t sent = 0;
        while (sent < element.size()) {
            if (packet.size() + 3 >= MTU) {
                out.payloads.push_back(packet);
                out.markers.push_back(false);
                packet = { 0x00 };
            }
            const size_t room = MTU - packet.size() - 2;
            const size_t chunk = std::min(room, element.size() - sent);
            appendLeb128(packet, chunk);
            packet.insert(packet.end(), element.begin() + static_cast<std::ptrdiff_t>(sent), element.begin() + static_cast<std::ptrdiff_t>(sent + chunk));
            sent += chunk;
            if (sent < element.size()) {
                packet[0] |= 0x40;
                out.payloads.push_back(packet);
                out.markers.push_back(false);
                packet = { 0x80 };
            }
        }
    }
    out.payloads.push_back(packet);
    out.markers.push_back(true);
    return out;
}

} // namespace VideoRtpSupport
