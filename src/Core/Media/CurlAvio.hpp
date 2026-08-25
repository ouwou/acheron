#pragma once

#ifdef ACHERON_HAVE_FFMPEG

#include <QString>

#include "Core/ProxyConfig.hpp"

extern "C" {
#include <libavformat/avio.h>
}

namespace Acheron {
namespace Core {
namespace Media {

namespace CurlAvio {

AVIOContext *create(const QString &url, const ProxyConfig &proxy, const AVIOInterruptCB &interrupt);
void destroy(AVIOContext *context);

[[nodiscard]] bool handlesScheme(const QString &url);

} // namespace CurlAvio

} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
