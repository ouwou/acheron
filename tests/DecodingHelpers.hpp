#pragma once

#include "Core/Media/VideoDecoder.hpp"

#include <QElapsedTimer>
#include <QSize>
#include <QThread>

#include <atomic>

namespace DecodingHelpers {

using Acheron::Discord::Voice::EncodedVideoFrame;
using Acheron::Discord::Voice::VideoCodec;

template <typename Frames>
void decodeAndWait(Acheron::Core::Media::VideoDecoder &decoder, VideoCodec codec, const Frames &frames, const std::atomic<int> &presented)
{
    constexpr int MOST_PUSHED_AHEAD = 20;
    constexpr qint64 LONGEST_WAIT_MS = 10000;
    constexpr unsigned long SETTLE_MS = 50;

    int pushed = 0;
    QElapsedTimer timer;
    timer.start();
    for (const auto &frame : frames) {
        decoder.push(codec, EncodedVideoFrame(*frame));
        pushed++;
        // the decoder discards its queue when it falls far behind
        while (pushed - presented > MOST_PUSHED_AHEAD && timer.elapsed() < LONGEST_WAIT_MS)
            QThread::msleep(1);
    }
    while (presented < pushed && timer.elapsed() < LONGEST_WAIT_MS)
        QThread::msleep(2);
    QThread::msleep(SETTLE_MS);
}

[[nodiscard]] inline bool isTheSizeSent(VideoCodec codec, const QSize &decoded, const QSize &sent)
{
    constexpr QSize AMD_AV1_BLOCK(64, 16);
    const QSize padding = decoded - sent;
    if (padding == QSize(0, 0))
        return true;
    return codec == VideoCodec::AV1 && padding.width() >= 0 && padding.height() >= 0 && padding.width() < AMD_AV1_BLOCK.width() && padding.height() < AMD_AV1_BLOCK.height();
}

} // namespace DecodingHelpers
