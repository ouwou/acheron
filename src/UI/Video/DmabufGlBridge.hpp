#pragma once

#include <QtGlobal>

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)

#define ACHERON_HAVE_DMABUF 1

#include <memory>

struct AVFrame;

namespace Acheron {
namespace UI {

// exported() is for any thread; the rest needs the OpenGL context current
class DmabufGlBridge
{
public:
    [[nodiscard]] static bool contextSupportsIt();

    // for the caller to free, or null. waits for decoding to finish
    [[nodiscard]] static AVFrame *exported(const AVFrame *frame);

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
    AVFrame *held;
};

} // namespace UI
} // namespace Acheron

#endif
