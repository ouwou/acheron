#include "DmabufGlBridge.hpp"

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)

#include <QLibrary>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <atomic>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
}

namespace Acheron {
namespace UI {

namespace {

// from EGL/egl.h and EGL/eglext.h
constexpr qint32 ATTRIBUTE_NONE = 0x3038;
constexpr qint32 ATTRIBUTE_EXTENSIONS = 0x3055;
constexpr qint32 ATTRIBUTE_HEIGHT = 0x3056;
constexpr qint32 ATTRIBUTE_WIDTH = 0x3057;
constexpr unsigned int TARGET_LINUX_DMA_BUF = 0x3270;
constexpr qint32 ATTRIBUTE_FOURCC = 0x3271;
constexpr qint32 ATTRIBUTE_PLANE_FD = 0x3272;
constexpr qint32 ATTRIBUTE_PLANE_OFFSET = 0x3273;
constexpr qint32 ATTRIBUTE_PLANE_PITCH = 0x3274;
constexpr qint32 ATTRIBUTE_MODIFIER_LOW = 0x3443;
constexpr qint32 ATTRIBUTE_MODIFIER_HIGH = 0x3444;

// drm_fourcc.h
constexpr quint32 FOURCC_R8 = 0x20203852;
constexpr quint32 FOURCC_GR88 = 0x38385247;
constexpr quint64 MODIFIER_LINEAR = 0;
constexpr quint64 MODIFIER_INVALID = 0x00ffffffffffffffULL;

// one decoding device serves the whole program, so EGL's verdict is settled once
std::atomic<bool> importRefused{ false };

} // namespace

struct DmabufGlBridge::Egl
{
    void *display = nullptr;
    void *(*createImage)(void *, void *, unsigned int, void *, const qint32 *) = nullptr;
    unsigned int (*destroyImage)(void *, void *) = nullptr;
    void (*imageTargetTexture)(unsigned int, void *) = nullptr;
    bool takesModifiers = false;

    static std::unique_ptr<Egl> resolve()
    {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (!context || !context->hasExtension(QByteArrayLiteral("GL_OES_EGL_image")))
            return nullptr;

        // a GLX context answers for any name, with a function that isnt there
        const auto currentDisplay = reinterpret_cast<void *(*)()>(QLibrary::resolve(QStringLiteral("EGL"), 1, "eglGetCurrentDisplay"));
        void *display = currentDisplay ? currentDisplay() : nullptr;
        if (!display)
            return nullptr;

        const auto queryString = reinterpret_cast<const char *(*)(void *, qint32)>(context->getProcAddress("eglQueryString"));
        const char *list = queryString ? queryString(display, ATTRIBUTE_EXTENSIONS) : nullptr;
        if (!list || !std::strstr(list, "EGL_EXT_image_dma_buf_import"))
            return nullptr;

        auto egl = std::make_unique<Egl>();
        egl->display = display;
        egl->takesModifiers = std::strstr(list, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        egl->createImage = reinterpret_cast<decltype(egl->createImage)>(context->getProcAddress("eglCreateImageKHR"));
        egl->destroyImage = reinterpret_cast<decltype(egl->destroyImage)>(context->getProcAddress("eglDestroyImageKHR"));
        egl->imageTargetTexture = reinterpret_cast<decltype(egl->imageTargetTexture)>(context->getProcAddress("glEGLImageTargetTexture2DOES"));
        if (!egl->createImage || !egl->destroyImage || !egl->imageTargetTexture)
            return nullptr;
        return egl;
    }
};

DmabufGlBridge::DmabufGlBridge()
    : held(av_frame_alloc()) {}

DmabufGlBridge::~DmabufGlBridge()
{
    av_frame_free(&held);
}

bool DmabufGlBridge::contextSupportsIt()
{
    return !importRefused.load(std::memory_order_relaxed) && Egl::resolve() != nullptr;
}

AVFrame *DmabufGlBridge::exported(const AVFrame *frame)
{
    AVFrame *exported = av_frame_alloc();
    exported->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(exported, frame, AV_HWFRAME_MAP_READ) < 0) {
        av_frame_free(&exported);
        return nullptr;
    }
    // a driver takes a plane at its allocated size and may refuse it at the picture's
    const auto *pool = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
    exported->width = pool->width;
    exported->height = pool->height;
    return exported;
}

bool DmabufGlBridge::update(const AVFrame *exported, unsigned int lumaTexture, unsigned int chromaTexture)
{
    release();
    if (!egl)
        egl = Egl::resolve();
    if (!egl)
        return false;

    const auto *descriptor = reinterpret_cast<const AVDRMFrameDescriptor *>(exported->data[0]);
    if (descriptor->nb_layers != 2 || descriptor->layers[0].format != FOURCC_R8 || descriptor->layers[1].format != FOURCC_GR88)
        return false;

    QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
    while (gl->glGetError() != GL_NO_ERROR) {}
    const unsigned int textures[2] = { lumaTexture, chromaTexture };
    bool made = true;
    for (int plane = 0; made && plane < 2; plane++) {
        const AVDRMPlaneDescriptor &layout = descriptor->layers[plane].planes[0];
        const AVDRMObjectDescriptor &object = descriptor->objects[layout.object_index];
        if (!egl->takesModifiers && object.format_modifier != MODIFIER_LINEAR && object.format_modifier != MODIFIER_INVALID) {
            made = false;
            break;
        }
        constexpr int MODIFIER_AT = 12;
        qint32 attributes[] = {
            ATTRIBUTE_WIDTH, plane == 0 ? exported->width : (exported->width + 1) / 2,
            ATTRIBUTE_HEIGHT, plane == 0 ? exported->height : (exported->height + 1) / 2,
            ATTRIBUTE_FOURCC, static_cast<qint32>(descriptor->layers[plane].format),
            ATTRIBUTE_PLANE_FD, object.fd,
            ATTRIBUTE_PLANE_OFFSET, static_cast<qint32>(layout.offset),
            ATTRIBUTE_PLANE_PITCH, static_cast<qint32>(layout.pitch),
            ATTRIBUTE_MODIFIER_LOW, static_cast<qint32>(object.format_modifier & 0xffffffff),
            ATTRIBUTE_MODIFIER_HIGH, static_cast<qint32>(object.format_modifier >> 32),
            ATTRIBUTE_NONE
        };
        if (!egl->takesModifiers || object.format_modifier == MODIFIER_INVALID)
            attributes[MODIFIER_AT] = ATTRIBUTE_NONE;
        void *image = egl->createImage(egl->display, nullptr, TARGET_LINUX_DMA_BUF, nullptr, attributes);
        if (!image) {
            made = false;
            break;
        }
        gl->glBindTexture(GL_TEXTURE_2D, textures[plane]);
        egl->imageTargetTexture(GL_TEXTURE_2D, image);
        made = gl->glGetError() == GL_NO_ERROR;
        egl->destroyImage(egl->display, image);
    }
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    if (!made)
        importRefused.store(true, std::memory_order_relaxed);
    return made && av_frame_ref(held, exported) >= 0;
}

void DmabufGlBridge::release()
{
    av_frame_unref(held);
}

} // namespace UI
} // namespace Acheron

#endif
