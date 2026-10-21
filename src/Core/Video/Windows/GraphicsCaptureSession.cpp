#include "Core/Video/Windows/GraphicsCaptureSession.hpp"

#if defined(Q_OS_WIN)

#include "Core/Video/Windows/Direct3D.hpp"

#include <QOperatingSystemVersion>

#include <atomic>
#include <thread>

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

std::atomic<bool> graphicsCaptureRuledOut{ false };

} // namespace

// making the filter loads everything it needs of Windows and captures nothing
bool graphicsCaptureUsable()
{
    static const bool initialises = [] {
        const AVFilter *filter = avfilter_get_by_name("gfxcapture");
        if (!filter || QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
            return false;
        AVFilterGraph *graph = avfilter_graph_alloc();
        AVFilterContext *made = nullptr;
        const bool initialised = avfilter_graph_create_filter(&made, filter, "probe", nullptr, nullptr, graph) >= 0;
        avfilter_graph_free(&graph);
        return initialised;
    }();
    return initialises && !graphicsCaptureRuledOut;
}

void ruleOutGraphicsCapture()
{
    graphicsCaptureRuledOut = true;
}

std::shared_ptr<GraphicsCaptureSession> GraphicsCaptureSession::start(const QByteArray &chain, const QByteArray &adapter, int *error)
{
    AVBufferRef *device = nullptr;
    *error = openDirect3dDevice(&device, adapter);
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

GraphicsCaptureSession::~GraphicsCaptureSession()
{
    avfilter_graph_free(&graph);
}

void GraphicsCaptureSession::abandon()
{
    const std::lock_guard lock(mutex);
    abandoned = true;
}

std::optional<int> GraphicsCaptureSession::endedWith()
{
    const std::lock_guard lock(mutex);
    return ended;
}

GraphicsCaptureSession::Taken GraphicsCaptureSession::take(AVFrame *into, std::chrono::steady_clock::time_point notBefore, std::chrono::steady_clock::time_point giveUpAt)
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

// Windows 10 hands over every screen refresh whatever rate was asked and keeps only two waiting
void GraphicsCaptureSession::run()
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

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
