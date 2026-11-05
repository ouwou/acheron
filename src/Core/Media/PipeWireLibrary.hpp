#pragma once

#include <dlfcn.h>

#include <pipewire/pipewire.h>

namespace Acheron {
namespace Core {
namespace Media {

[[nodiscard]] inline bool pipeWireReady()
{
    static const bool ready = [] {
        // a portable build (IMPLIB_GEN in linuxcapture.cmake) loads libpipewire on its first call and aborts where there is none
        if (!dlopen("libpipewire-0.3.so.0", RTLD_LAZY))
            return false;
        pw_init(nullptr, nullptr);
        return true;
    }();
    return ready;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
