#include "Core/Video/Windows/GraphicsCaptureSession.hpp"

#if defined(Q_OS_WIN)

#include <QOperatingSystemVersion>

#include <atomic>

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

std::atomic<bool> graphicsCaptureRuledOut{ false };

} // namespace

// Making the filter loads everything it needs of Windows, and captures nothing.
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

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
