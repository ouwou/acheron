#include "Core/Media/InputConfinement.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#include "Core/Logging.hpp"

#include <QUrl>

#include <utility>

namespace Acheron {
namespace Core {
namespace Media {

namespace {

const char *const DOWNLOAD_PROTOCOLS = "http,https,tls,tcp,httpproxy";
const char *const LOCAL_PROTOCOLS = "file";

bool isDownload(const QString &input)
{
    const QString scheme = QUrl(input).scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

} // namespace

void InputConfinement::apply(AVFormatContext *format, AVDictionary **options, const QString &input)
{
    av_dict_set(options, "protocol_whitelist", isDownload(input) ? DOWNLOAD_PROTOCOLS : LOCAL_PROTOCOLS, 0);

    // with the caller's own I/O in place ffmpeg opens nothing for the input itself
    inputOpened = format->pb != nullptr;
    open = format->io_open;
    format->opaque = this;
    format->io_open = &InputConfinement::openOnlyTheInput;
}

int InputConfinement::openOnlyTheInput(AVFormatContext *format, AVIOContext **io, const char *url, int flags, AVDictionary **options)
{
    auto *confinement = static_cast<InputConfinement *>(format->opaque);
    if (std::exchange(confinement->inputOpened, true)) {
        qCWarning(LogVideo) << "Refusing to open" << url << "on behalf of the media being played";
        return AVERROR(EPERM);
    }
    return confinement->open(format, io, url, flags, options);
}

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
