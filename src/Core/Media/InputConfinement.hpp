#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QString>

extern "C" {
#include <libavformat/avformat.h>
}

namespace Acheron {
namespace Core {
namespace Media {

// an attachment named like a video can be an HLS or DASH playlist, an SDP description or a QuickTime file
// with outside references, which would have ffmpeg connect to any address, past any proxy
class InputConfinement
{
public:
    // call between avformat_alloc_context() and avformat_open_input(), which must be given `options`
    void apply(AVFormatContext *format, AVDictionary **options, const QString &input);

private:
    static int openOnlyTheInput(AVFormatContext *format, AVIOContext **io, const char *url, int flags, AVDictionary **options);

    decltype(AVFormatContext::io_open) open = nullptr;
    bool inputOpened = false;
};

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
