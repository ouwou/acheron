#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QByteArray>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/PictureConverter.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

// Windows.Graphics.Capture, through ffmpeg's gfxcapture filter: whether it can be used, and the
// capture of one source by it.

// Capturing a window by its handle came with Windows 10 1903 and the filter with ffmpeg 8.1.
// False as well once a capture has found out that something else it needs is missing.
[[nodiscard]] bool graphicsCaptureUsable();
void ruleOutGraphicsCapture();

// Graphics capture, read on a thread of its own. Reading it does not come back until the
// source shows something new, which a minimised window never does, and ffmpeg has no way to
// call a read off. So a capture that is no longer wanted is only abandoned: its thread frees
// it when the read it is in returns, which is when the window is next shown or is closed.
class GraphicsCaptureSession
{
public:
    enum class Taken {
        Picture,
        Nothing,
        Ended,
    };

    // Null when the capture could not be made, with ffmpeg's reason in `error`.
    static std::shared_ptr<GraphicsCaptureSession> start(const QByteArray &chain, const QByteArray &adapter, int *error)
    {
        AVBufferRef *device = nullptr;
        *error = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, adapter.isEmpty() ? nullptr : adapter.constData(), nullptr, 0);
        if (*error < 0)
            return nullptr;

        auto session = std::make_shared<GraphicsCaptureSession>();
        *error = assemble(chain, device, &session->graph, &session->sink);
        av_buffer_unref(&device);
        if (*error < 0)
            return nullptr;
        std::thread([session] { session->run(); }).detach();
        return session;
    }

    ~GraphicsCaptureSession() { avfilter_graph_free(&graph); }

    void abandon()
    {
        const std::lock_guard lock(mutex);
        abandoned = true;
    }

    // What ffmpeg ended the capture with, or zero while it runs: the end of the stream when
    // the window was closed or hidden, an error otherwise.
    [[nodiscard]] int endedWith()
    {
        const std::lock_guard lock(mutex);
        return ended;
    }

    // The newest picture not taken yet, once it is `notBefore`. Waits for one until `giveUpAt`.
    Taken take(AVFrame *into, std::chrono::steady_clock::time_point notBefore, std::chrono::steady_clock::time_point giveUpAt)
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (ended)
                return Taken::Ended;
            const auto now = std::chrono::steady_clock::now();
            if (fresh && now >= notBefore) {
                av_frame_unref(into);
                av_frame_move_ref(into, latest.get());
                fresh = false;
                return Taken::Picture;
            }
            if (now >= giveUpAt)
                return Taken::Nothing;
            arrived.wait_until(lock, fresh ? notBefore : giveUpAt);
        }
    }

private:
    // Windows 10 hands over a picture for every refresh of the screen whatever rate it was
    // asked for, and keeps only two waiting. All are read, so that the one taken is the newest.
    void run()
    {
        const Media::FramePointer picture(av_frame_alloc());
        for (;;) {
            const int got = av_buffersink_get_frame(sink, picture.get());
            const std::lock_guard lock(mutex);
            if (abandoned)
                break;
            if (got < 0) {
                ended = got;
                break;
            }
            av_frame_unref(latest.get());
            av_frame_move_ref(latest.get(), picture.get());
            fresh = true;
            arrived.notify_all();
        }
        arrived.notify_all();
        avfilter_graph_free(&graph);
    }

    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    std::mutex mutex;
    std::condition_variable arrived;
    Media::FramePointer latest{ av_frame_alloc() };
    bool fresh = false;
    bool abandoned = false;
    int ended = 0;
};

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
