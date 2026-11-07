#pragma once

#include <QtGlobal>

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)

#define ACHERON_HAVE_DMABUF 1

#include <memory>

#include "Core/Media/FfmpegPointers.hpp"

namespace Acheron {
namespace UI {

// exported() is for any thread; the rest needs the OpenGL context current
class DmabufGlBridge
{
public:
    [[nodiscard]] static bool contextSupportsIt();

    // waits for decoding to finish
    [[nodiscard]] static Core::Media::FramePointer exported(const AVFrame *frame);

    DmabufGlBridge();
    ~DmabufGlBridge();
    DmabufGlBridge(const DmabufGlBridge &) = delete;
    DmabufGlBridge &operator=(const DmabufGlBridge &) = delete;

    bool update(const AVFrame *exported, unsigned int lumaTexture, unsigned int chromaTexture);

    // the textures are the decoder's surface, held until the next frame or this
    void release();

private:
    struct Egl;

    std::unique_ptr<Egl> egl;
    const Core::Media::FramePointer held{ av_frame_alloc() };
};

} // namespace UI
} // namespace Acheron

#endif
