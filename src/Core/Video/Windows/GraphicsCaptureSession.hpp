#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QByteArray>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/PictureConverter.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

// Windows 10 1903 and ffmpeg 8.1
[[nodiscard]] bool graphicsCaptureUsable();
void ruleOutGraphicsCapture();

// a read doesnt return until the source shows something new, which a minimised window never does,
// and ffmpeg cant cancel it. so an unwanted capture is abandoned, and its thread frees it when the
// read returns
class GraphicsCaptureSession
{
public:
    enum class Taken {
        Picture,
        Nothing,
        Ended,
    };

    static std::shared_ptr<GraphicsCaptureSession> start(const QByteArray &chain, const QByteArray &adapter, int *error);
    ~GraphicsCaptureSession();

    void abandon();
    [[nodiscard]] std::optional<int> endedWith();
    Taken take(AVFrame *into, std::chrono::steady_clock::time_point notBefore, std::chrono::steady_clock::time_point giveUpAt);

private:
    void run();

    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    std::mutex mutex;
    std::condition_variable arrived;
    Media::FramePointer latest{ av_frame_alloc() };
    bool fresh = false;
    bool abandoned = false;
    std::optional<int> ended;
};

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
