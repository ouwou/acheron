#include "CardWorker.hpp"

#ifdef Q_OS_WIN

#include "GlTextures.hpp"

#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>

#include <algorithm>
#include <map>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace UI {

namespace {

constexpr GLuint64 FINISH_TIMEOUT_NS = 100'000'000;

} // namespace

std::shared_ptr<CardWorker> CardWorker::sharingWith(QOpenGLContext *layerContext)
{
    static std::map<QOpenGLContextGroup *, std::vector<std::weak_ptr<CardWorker>>> groups;
    for (auto group = groups.begin(); group != groups.end();) {
        std::vector<std::weak_ptr<CardWorker>> &workers = group->second;
        workers.erase(std::remove_if(workers.begin(), workers.end(), [](const std::weak_ptr<CardWorker> &worker) { return worker.expired(); }), workers.end());
        group = workers.empty() ? groups.erase(group) : std::next(group);
    }

    // every surface holds one reference to its worker, and `candidate` one more
    std::vector<std::weak_ptr<CardWorker>> &workers = groups[layerContext->shareGroup()];
    std::shared_ptr<CardWorker> least;
    long leastServed = 0;
    for (const std::weak_ptr<CardWorker> &entry : workers) {
        const std::shared_ptr<CardWorker> candidate = entry.lock();
        const long served = candidate.use_count() - 1;
        if (!least || served < leastServed) {
            least = candidate;
            leastServed = served;
        }
    }
    if (least && (leastServed < SURFACES_PER_WORKER || static_cast<int>(workers.size()) >= MAX_WORKERS))
        return least;

    std::shared_ptr<CardWorker> worker(new CardWorker(layerContext));
    if (!worker->thread.isRunning())
        return least;
    workers.push_back(worker);
    return worker;
}

CardWorker::CardWorker(QOpenGLContext *layerContext)
{
    context.setFormat(layerContext->format());
    context.setShareContext(layerContext);
    surface.setFormat(layerContext->format());
    surface.create();
    if (!surface.isValid() || !context.create())
        return;
    context.moveToThread(&thread);
    thread.setObjectName(QStringLiteral("VideoHandOver"));
    thread.start();
}

CardWorker::~CardWorker()
{
    if (!thread.isRunning())
        return;
    // the context goes back to the thread that is about to destroy it
    run([this, home = QThread::currentThread()] {
        context.doneCurrent();
        context.moveToThread(home);
    });
    thread.quit();
    thread.wait();
}

void CardWorker::enter()
{
    if (QOpenGLContext::currentContext() != &context)
        context.makeCurrent(&surface);
}

void CardWorker::finish()
{
    QOpenGLExtraFunctions *extra = context.extraFunctions();
    if (!context.hasExtension(QByteArrayLiteral("GL_ARB_sync"))) {
        extra->glFinish();
        return;
    }
    const GLsync fence = extra->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    extra->glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, FINISH_TIMEOUT_NS);
    extra->glDeleteSync(fence);
}

bool CardPicture::render(const AVFrame *frame)
{
    if (closed)
        return true;
    const QSize size(frame->width, frame->height);
    if (!bridge.update(frame, size) || !bridge.lock())
        return false;

    QOpenGLFunctions *gl = worker->gl();
    if (!framebuffer)
        gl->glGenFramebuffers(1, &framebuffer);
    if (!back.name) {
        gl->glGenTextures(1, &back.name);
        gl->glBindTexture(GL_TEXTURE_2D, back.name);
        setSampling(gl, GL_LINEAR);
    }
    gl->glBindTexture(GL_TEXTURE_2D, back.name);
    if (back.size != size) {
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        back.size = size;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bridge.texture(), 0);
    gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size.width(), size.height());
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    worker->finish();

    std::lock_guard lock(mutex);
    std::swap(back, ready);
    readyIsNew = true;
    return true;
}

unsigned int CardPicture::acquire()
{
    std::lock_guard lock(mutex);
    if (readyIsNew) {
        std::swap(front, ready);
        readyIsNew = false;
        frontIsCurrent = true;
    }
    return frontIsCurrent ? front.name : 0;
}

void CardPicture::discard()
{
    std::lock_guard lock(mutex);
    readyIsNew = false;
    frontIsCurrent = false;
}

void CardPicture::close()
{
    worker->run([this] {
        closed = true;
        bridge.release();
        QOpenGLFunctions *gl = worker->gl();
        for (Texture *texture : { &front, &ready, &back }) {
            if (texture->name)
                gl->glDeleteTextures(1, &texture->name);
            *texture = {};
        }
        if (framebuffer)
            gl->glDeleteFramebuffers(1, &framebuffer);
        framebuffer = 0;
    });
}

} // namespace UI
} // namespace Acheron

#endif
