#pragma once

#include <dlfcn.h>

#include <pipewire/pipewire.h>

namespace Acheron {
namespace Core {
namespace Media {

// a portable build (IMPLIB_GEN in linuxcapture.cmake) loads libpipewire on first call and aborts where
// there is none
[[nodiscard]] inline bool pipeWireCanBeCalled()
{
    static const bool loadable = dlopen("libpipewire-0.3.so.0", RTLD_LAZY) != nullptr;
    return loadable;
}

[[nodiscard]] inline bool pipeWireReady()
{
    static const bool ready = [] {
        if (!pipeWireCanBeCalled())
            return false;
        pw_init(nullptr, nullptr);
        return true;
    }();
    return ready;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
