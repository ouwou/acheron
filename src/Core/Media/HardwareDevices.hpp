#pragma once

#include <QtGlobal>

extern "C" {
#include <libavutil/hwcontext.h>
}

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <dlfcn.h>
#endif

namespace Acheron {
namespace Core {
namespace Media {

// the prebuilt ffmpeg loads libva on the first VAAPI request and aborts when it is missing
[[nodiscard]] inline bool hardwareDeviceCanBeAskedFor(AVHWDeviceType type)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (type != AV_HWDEVICE_TYPE_VAAPI)
        return true;

    static const bool loadable = [] {
        // an ffmpeg linked against libva normally has loaded it already
        if (void *linked = dlopen("libva.so.2", RTLD_NOLOAD | RTLD_LAZY)) {
            dlclose(linked);
            return true;
        }
        for (const char *library : { "libva.so.2", "libva-drm.so.2", "libva-x11.so.2" }) {
            if (!dlopen(library, RTLD_LAZY | RTLD_GLOBAL))
                return false;
        }
        return true;
    }();
    return loadable;
#else
    Q_UNUSED(type);
    return true;
#endif
}

} // namespace Media
} // namespace Core
} // namespace Acheron
