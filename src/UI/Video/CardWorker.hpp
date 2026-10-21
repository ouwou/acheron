#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSize>
#include <QThread>

#include <memory>
#include <mutex>

#include "D3D11GlBridge.hpp"

class QOpenGLFunctions;
struct AVFrame;

namespace Acheron {
namespace UI {

// handing a picture from Direct3D to OpenGL waits on the card, milliseconds per frame per surface, so
// the wait happens here in a context sharing textures with the video layers
class CardWorker
{
public:
    static constexpr int SURFACES_PER_WORKER = 3;
    static constexpr int MAX_WORKERS = 3;

    // needs the layer's context current
    static std::shared_ptr<CardWorker> sharingWith(QOpenGLContext *layerContext);
    ~CardWorker();

    template <typename Job>
    void post(Job job)
    {
        QMetaObject::invokeMethod(&context, [this, job = std::move(job)] {
            enter();
            job(); }, Qt::QueuedConnection);
    }

    // blocks. not from the worker's thread
    template <typename Job>
    void run(Job job)
    {
        QMetaObject::invokeMethod(&context, [this, &job] {
            enter();
            job(); }, Qt::BlockingQueuedConnection);
    }

    [[nodiscard]] QOpenGLFunctions *gl() { return context.functions(); }

    void finish();

private:
    explicit CardWorker(QOpenGLContext *layerContext);
    void enter();

    QThread thread;
    QOpenGLContext context;
    QOffscreenSurface surface;
};

class CardPicture
{
public:
    explicit CardPicture(std::shared_ptr<CardWorker> worker) : worker(std::move(worker)) {}

    template <typename Job>
    void post(Job job)
    {
        worker->post(std::move(job));
    }

    // on the worker's thread
    bool render(const AVFrame *frame);
    // on the layer's thread
    [[nodiscard]] unsigned int acquire();
    void discard();
    // on the layer's thread. waits for the worker to let go of everything
    void close();

private:
    struct Texture
    {
        unsigned int name = 0;
        QSize size;
    };

    std::shared_ptr<CardWorker> worker;
    // the worker's thread only
    D3D11GlBridge bridge;
    unsigned int framebuffer = 0;
    Texture back;
    bool closed = false;

    std::mutex mutex;
    Texture ready;
    // the layer's thread only, apart from the exchange with `ready`
    Texture front;
    bool readyIsNew = false;
    bool frontIsCurrent = false;
};

} // namespace UI
} // namespace Acheron

#endif
