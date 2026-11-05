#pragma once

#include "Core/Media/FfmpegPointers.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace TestPictures {

struct YuvSamples
{
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

inline constexpr YuvSamples WHITE{ 235, 128, 128 };
inline constexpr YuvSamples BLACK{ 16, 128, 128 };
inline constexpr YuvSamples RED_BT709{ 63, 102, 240 };
inline constexpr YuvSamples GREEN_BT709{ 173, 42, 26 };
inline constexpr YuvSamples RED_BT601{ 81, 90, 240 };

inline Acheron::Core::Media::FramePointer solidFrame(YuvSamples colour, int width, int height)
{
    Acheron::Core::Media::FramePointer frame(av_frame_alloc());
    if (!frame || Acheron::Core::Media::prepareWritable(frame.get(), AV_PIX_FMT_YUV420P, QSize(width, height)) < 0)
        return nullptr;
    const auto chromaWidth = static_cast<size_t>((width + 1) / 2);
    for (int row = 0; row < height; row++)
        std::memset(frame->data[0] + row * frame->linesize[0], colour.y, static_cast<size_t>(width));
    for (int row = 0; row < (height + 1) / 2; row++) {
        std::memset(frame->data[1] + row * frame->linesize[1], colour.u, chromaWidth);
        std::memset(frame->data[2] + row * frame->linesize[2], colour.v, chromaWidth);
    }
    return frame;
}

// planar YUV of any subsampling, or NV12
inline void paintMovement(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            auto value = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < width / 4 && y < height / 6) {
                noise = noise * 1664525u + 1013904223u;
                value = static_cast<uint8_t>(96 + (noise >> 26));
            }
            row[x] = value;
        }
    }

    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(picture->format));
    const int chromaWidth = AV_CEIL_RSHIFT(width, layout->log2_chroma_w);
    const int chromaHeight = AV_CEIL_RSHIFT(height, layout->log2_chroma_h);
    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < chromaHeight; y++) {
        for (int x = 0; x < chromaWidth; x++) {
            const auto u = static_cast<uint8_t>(128 + y / 4 + index * 2);
            const auto v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

} // namespace TestPictures
