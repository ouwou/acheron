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

// Handing a picture from Direct3D to OpenGL makes the caller wait on the graphics card, a
// few milliseconds for every frame of every surface. The waiting is done on this thread, in
// a context that shares its textures with the widgets', so that all a widget has to do is
// draw a finished texture. A worker takes its surfaces' frames one after another, so a call
// full of cameras is spread over a few of them.
class CardWorker
{
public:
    static constexpr int SURFACES_PER_WORKER = 3;
    static constexpr int MAX_WORKERS = 3;

    // Needs the widget's context current. Null when a context sharing with it cannot be made.
    static std::shared_ptr<CardWorker> sharingWith(QOpenGLContext *widgetContext);
    ~CardWorker();

    // Runs the job on the worker's thread with its context current.
    template <typename Job>
    void post(Job job)
    {
        QMetaObject::invokeMethod(&context, [this, job = std::move(job)] {
            enter();
            job(); }, Qt::QueuedConnection);
    }

    // The same, waiting for it. Not to be called from the worker's thread.
    template <typename Job>
    void run(Job job)
    {
        QMetaObject::invokeMethod(&context, [this, &job] {
            enter();
            job(); }, Qt::BlockingQueuedConnection);
    }

    [[nodiscard]] QOpenGLFunctions *gl() { return context.functions(); }

    // Waits until the card has done everything asked of it so far, which is when another
    // context may rely on the result. On the worker's thread.
    void finish();

private:
    explicit CardWorker(QOpenGLContext *widgetContext);
    void enter();

    QThread thread;
    QOpenGLContext context;
    QOffscreenSurface surface;
};

// One surface's pictures from the card: made by the worker, drawn by the widget. Three
// textures go round, so that the worker always has one to fill, the widget one to draw, and a
// finished picture can wait between the two.
class CardPicture
{
public:
    explicit CardPicture(std::shared_ptr<CardWorker> worker) : worker(std::move(worker)) {}

    template <typename Job>
    void post(Job job)
    {
        worker->post(std::move(job));
    }

    // On the worker's thread. False when this machine cannot do it for such a frame.
    bool render(const AVFrame *frame, bool bt709, bool fullRange);
    // On the widget's thread: the texture to draw, taking the newest finished picture. Zero
    // while there is none.
    [[nodiscard]] unsigned int acquire();
    // Forgets the pictures made so far.
    void discard();
    // On the widget's thread. Waits for the worker to let go of everything.
    void close();

private:
    struct Texture
    {
        unsigned int name = 0;
        QSize size;
    };

    std::shared_ptr<CardWorker> worker;
    // The worker's thread only.
    D3D11GlBridge bridge;
    unsigned int framebuffer = 0;
    Texture back;
    bool closed = false;

    std::mutex mutex;
    Texture ready;
    // The widget's thread only, apart from the exchange with `ready`.
    Texture front;
    bool readyIsNew = false;
    bool frontIsCurrent = false;
};

} // namespace UI
} // namespace Acheron

#endif
