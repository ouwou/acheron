#include "VideoSurface.hpp"

#include "D3D11GlBridge.hpp"
#include "DmabufGlBridge.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/RgbScaler.hpp"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QMatrix3x3>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWindow>
#include <QPlatformSurfaceEvent>
#include <QPainter>
#include <QPen>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace UI {

namespace {

const char *const GPU_DRAWING_KEY = "video/gpu_drawing";
// Not in the settings window: a way out for a machine whose drivers get this wrong.
const char *const DIRECT_FRAMES_KEY = "video/direct_frames";

constexpr int RESCALE_DELAY_MS = 120;
// Drawn over video, so it does not follow the theme: the green Discord rings a speaker with.
const QColor SPEAKING_RING(0x23, 0xA5, 0x5A);

using Core::Media::isFullRange;
using Core::Media::usesBt709;

QSize fitted(QSize content, QSize bounds)
{
    if (content.isEmpty() || bounds.isEmpty())
        return {};
    return content.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

// The layouts the shader reads directly: 8-bit 4:2:0, planar or with interleaved chroma.
bool isUploadable(int format)
{
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

// Whether an OpenGL context can be had at all. Remote sessions and some old machines have none.
bool openGlWorks()
{
    static const bool works = [] {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            return false;
        QOpenGLContext context;
        return context.create();
    }();
    return works;
}

} // namespace

#ifdef Q_OS_WIN

namespace {

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
    static std::shared_ptr<CardWorker> sharingWith(QOpenGLContext *widgetContext)
    {
        static std::map<QOpenGLContextGroup *, std::vector<std::weak_ptr<CardWorker>>> groups;
        for (auto group = groups.begin(); group != groups.end();) {
            std::vector<std::weak_ptr<CardWorker>> &workers = group->second;
            workers.erase(std::remove_if(workers.begin(), workers.end(), [](const std::weak_ptr<CardWorker> &worker) { return worker.expired(); }), workers.end());
            group = workers.empty() ? groups.erase(group) : std::next(group);
        }

        // Every surface holds one reference to its worker, and `candidate` one more.
        std::vector<std::weak_ptr<CardWorker>> &workers = groups[widgetContext->shareGroup()];
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

        std::shared_ptr<CardWorker> worker(new CardWorker(widgetContext));
        if (!worker->thread.isRunning())
            return least;
        workers.push_back(worker);
        return worker;
    }

    ~CardWorker()
    {
        if (!thread.isRunning())
            return;
        // The context goes back to the thread that is about to destroy it.
        run([this, home = QThread::currentThread()] {
            context.doneCurrent();
            context.moveToThread(home);
        });
        thread.quit();
        thread.wait();
    }

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
    void finish()
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

private:
    static constexpr GLuint64 FINISH_TIMEOUT_NS = 100'000'000;

    explicit CardWorker(QOpenGLContext *widgetContext)
    {
        context.setFormat(widgetContext->format());
        context.setShareContext(widgetContext);
        surface.setFormat(widgetContext->format());
        surface.create();
        if (!surface.isValid() || !context.create())
            return;
        context.moveToThread(&thread);
        thread.setObjectName(QStringLiteral("VideoHandOver"));
        thread.start();
    }

    void enter()
    {
        if (QOpenGLContext::currentContext() != &context)
            context.makeCurrent(&surface);
    }

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
    explicit CardPicture(std::shared_ptr<CardWorker> worker)
        : worker(std::move(worker)) {}

    template <typename Job>
    void post(Job job)
    {
        worker->post(std::move(job));
    }

    // On the worker's thread. False when this machine cannot do it for such a frame.
    bool render(const AVFrame *frame, bool bt709, bool fullRange)
    {
        if (closed)
            return true;
        // Made at the picture's own size; the widget's drawing scales it.
        const QSize size(frame->width, frame->height);
        if (!bridge.update(frame, size, bt709, fullRange) || !bridge.lock())
            return false;

        QOpenGLFunctions *gl = worker->gl();
        if (!framebuffer)
            gl->glGenFramebuffers(1, &framebuffer);
        if (!back.name) {
            gl->glGenTextures(1, &back.name);
            gl->glBindTexture(GL_TEXTURE_2D, back.name);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        gl->glBindTexture(GL_TEXTURE_2D, back.name);
        if (back.size != size) {
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            back.size = size;
        }
        // A copy on the card, out of the texture Direct3D drew into.
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

    // On the widget's thread: the texture to draw, taking the newest finished picture. Zero
    // while there is none.
    [[nodiscard]] unsigned int acquire()
    {
        std::lock_guard lock(mutex);
        if (readyIsNew) {
            std::swap(front, ready);
            readyIsNew = false;
            frontIsCurrent = true;
        }
        return frontIsCurrent ? front.name : 0;
    }

    // Forgets the pictures made so far.
    void discard()
    {
        std::lock_guard lock(mutex);
        readyIsNew = false;
        frontIsCurrent = false;
    }

    // On the widget's thread. Waits for the worker to let go of everything.
    void close()
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

} // namespace

#endif // Q_OS_WIN

// Holds the latest decoded frame for the surface. For the software path it also converts each
// frame to RGB at the widget's size: the decoder's thread writes into `back` and swaps it with
// `front`, and the widget only ever reads `front`, under the same lock, so neither image is
// shared and none is allocated per frame. A frame still on the graphics card is passed to the
// worker that turns it into a texture, and the widget hears of it when that is done; a VAAPI
// one is kept with its DMA-BUFs, for the widget to draw from.
class VideoSurface::Sink : public Core::Media::VideoFrameSink, public std::enable_shared_from_this<VideoSurface::Sink>
{
public:
    Sink(VideoSurface *owner, bool gpu)
        : owner(owner), gpu(gpu), last(av_frame_alloc()), handing(av_frame_alloc()) {}

    ~Sink() override
    {
        sws_freeContext(normalizer);
        av_frame_free(&last);
        av_frame_free(&handing);
        av_frame_free(&exported);
    }

    void present(const AVFrame *frame) override
    {
#ifdef ACHERON_HAVE_DMABUF
        // Waits for the card to finish the picture: on this thread, the decoder's, and not
        // under the lock.
        AVFrame *handed = frame->format == AV_PIX_FMT_VAAPI ? DmabufGlBridge::exported(frame) : nullptr;
#endif
        std::lock_guard lock(mutex);
#ifdef ACHERON_HAVE_DMABUF
        av_frame_free(&exported);
        exported = handed;
#endif
        if (!owner)
            return;

        store(frame);
        serial++;
#ifdef Q_OS_WIN
        // Without a card to hand it to, the surface hears of the frame as of any other: it
        // may have no layer yet to draw it in.
        if (last->hw_frames_ctx && card) {
            requestHandOver();
            return;
        }
#endif
        if (!gpu)
            convert();
        announce();
    }

#ifdef Q_OS_WIN
    // Set by the layer that draws once it can draw the card's pictures, and cleared when it
    // no longer can.
    void setCard(std::shared_ptr<CardPicture> picture)
    {
        std::lock_guard lock(mutex);
        card = std::move(picture);
        hardwareFramesUsable.store(card != nullptr, std::memory_order_relaxed);
        // The frame held may be one the card before this one never got to.
        if (shown && last->hw_frames_ctx)
            requestHandOver();
    }
#endif

#ifdef ACHERON_HAVE_DMABUF
    // Set by the layer that draws once it can draw the card's pictures, and cleared when it
    // no longer can.
    void setCardUsable(bool usable)
    {
        hardwareFramesUsable.store(usable, std::memory_order_relaxed);
    }

    // The frame as DMA-BUFs, when it is one from the card that could be had so. For the reader
    // in withFrame().
    [[nodiscard]] const AVFrame *exportedFrame() const { return exported; }

    // For the reader in withFrame(), when the frame is on the card and cannot be drawn from
    // there: it is brought into memory as the decoder would have brought it.
    void readBack()
    {
        AVFrame *downloaded = av_frame_alloc();
        if (av_hwframe_transfer_data(downloaded, last, 0) >= 0 && av_frame_copy_props(downloaded, last) >= 0)
            store(downloaded);
        av_frame_free(&downloaded);
        av_frame_free(&exported);
    }
#endif

    bool acceptsHardwareFrames(int format) const override
    {
#ifdef ACHERON_HAVE_DMABUF
        return format == AV_PIX_FMT_VAAPI && hardwareFramesUsable.load(std::memory_order_relaxed);
#else
        return format == AV_PIX_FMT_D3D11 && hardwareFramesUsable.load(std::memory_order_relaxed);
#endif
    }

    // Whether the surface is on screen. The card's pictures are not made ready for one that
    // is not.
    void setShown(bool value)
    {
        std::lock_guard lock(mutex);
        shown = value;
#ifdef Q_OS_WIN
        if (shown && last->hw_frames_ctx)
            requestHandOver();
#endif
    }

    void detach()
    {
        std::lock_guard lock(mutex);
        owner = nullptr;
    }

    [[nodiscard]] bool hasPicture()
    {
        std::lock_guard lock(mutex);
        return last->width > 0;
    }

    // A picture left on the card holds its decoder's surfaces there. One that is not being
    // shown is kept in memory instead.
    void keepInMemory()
    {
        std::lock_guard lock(mutex);
        if (!last->hw_frames_ctx)
            return;
        AVFrame *downloaded = av_frame_alloc();
        if (av_hwframe_transfer_data(downloaded, last, 0) >= 0 && av_frame_copy_props(downloaded, last) >= 0) {
            store(downloaded);
            av_frame_free(&exported);
            serial++;
        }
        av_frame_free(&downloaded);
    }

    void setTarget(QSize devicePixels, qreal devicePixelRatio)
    {
        std::lock_guard lock(mutex);
        target = devicePixels;
        pixelRatio = devicePixelRatio;
    }

    // Redraws the last frame at the current size, for when no new frame is coming to do it.
    void rescale()
    {
        std::lock_guard lock(mutex);
        if (!gpu && last->width > 0)
            convert();
    }

    void forget()
    {
        std::lock_guard lock(mutex);
        av_frame_unref(last);
        av_frame_free(&exported);
        front = QImage();
        serial++;
    }

    [[nodiscard]] QSize pictureSize()
    {
        std::lock_guard lock(mutex);
        return fitted(displaySize(), target);
    }

    template <typename Painter>
    void withFront(Painter paint)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        paint(front);
    }

    // The latest frame and a number that changes whenever it does. The frame has no picture
    // (zero width) when there is nothing to show.
    template <typename Reader>
    void withFrame(Reader read)
    {
        std::lock_guard lock(mutex);
        updatePending = false;
        read(last, serial, displaySize());
    }

private:
    // With the lock held.
    void announce()
    {
        if (!owner || updatePending)
            return;
        updatePending = true;
        QMetaObject::invokeMethod(owner, [surface = owner] { surface->onFrameArrived(); }, Qt::QueuedConnection);
    }

#ifdef Q_OS_WIN
    // With the lock held. One request covers every frame that arrives before the worker gets
    // to it.
    void requestHandOver()
    {
        if (!card || !shown || handOverPending)
            return;
        handOverPending = true;
        card->post([self = shared_from_this(), picture = card] { self->handOver(*picture); });
    }

    // On the worker's thread.
    void handOver(CardPicture &picture)
    {
        {
            std::lock_guard lock(mutex);
            handOverPending = false;
            if (last->hw_frames_ctx)
                av_frame_ref(handing, last);
        }
        if (!handing->hw_frames_ctx)
            return;
        const bool made = picture.render(handing, usesBt709(handing), isFullRange(handing));
        av_frame_unref(handing);

        std::lock_guard lock(mutex);
        if (made)
            announce();
        else
            hardwareFramesUsable.store(false, std::memory_order_relaxed);
    }
#endif

    [[nodiscard]] QSize displaySize() const
    {
        if (last->width <= 0 || last->height <= 0)
            return {};
        const AVRational aspect = last->sample_aspect_ratio;
        if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den)
            return QSize(static_cast<int>(static_cast<qint64>(last->width) * aspect.num / aspect.den), last->height);
        return QSize(last->width, last->height);
    }

    void store(const AVFrame *frame)
    {
        av_frame_unref(last);
        if (!gpu || isUploadable(frame->format) || frame->hw_frames_ctx) {
            av_frame_ref(last, frame);
            return;
        }

        // Anything else, such as 10-bit video, is brought to 8-bit 4:2:0 here so that the
        // shader has one family of layouts to deal with.
        last->format = AV_PIX_FMT_YUV420P;
        last->width = frame->width;
        last->height = frame->height;
        if (av_frame_get_buffer(last, 0) < 0) {
            av_frame_unref(last);
            return;
        }
        normalizer = sws_getCachedContext(normalizer, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
        if (!normalizer) {
            av_frame_unref(last);
            return;
        }
        sws_scale(normalizer, frame->data, frame->linesize, 0, frame->height, last->data, last->linesize);
        last->colorspace = frame->colorspace;
        last->color_range = frame->color_range;
        last->sample_aspect_ratio = frame->sample_aspect_ratio;
    }

    void convert()
    {
        const QSize size = fitted(displaySize(), target);
        if (size.isEmpty())
            return;

        if (back.size() != size)
            back = Core::Media::RgbScaler::allocate(size, QImage::Format_RGB32);
        if (!scaler.scale(last, back))
            return;
        back.setDevicePixelRatio(pixelRatio);
        back.swap(front);
    }

    std::mutex mutex;
    VideoSurface *owner;
    bool gpu;
    bool shown = false;
    std::atomic<bool> hardwareFramesUsable{ false };
#ifdef Q_OS_WIN
    std::shared_ptr<CardPicture> card;
    bool handOverPending = false;
#endif
    AVFrame *last;
    // The worker's own reference to the frame it is turning into a texture.
    AVFrame *handing;
    // `last` as DMA-BUFs, when it is a VAAPI frame.
    AVFrame *exported = nullptr;
    quint64 serial = 0;
    Core::Media::RgbScaler scaler;
    SwsContext *normalizer = nullptr;
    QSize target;
    qreal pixelRatio = 1.0;
    QImage front;
    QImage back;
    bool updatePending = false;
};

// Draws the picture with the graphics card. A frame in memory goes up as textures, and a shader
// converts them to RGB while the card scales them to the widget. A frame the decoder left on
// the card arrives as a finished texture, which is only drawn, or, from VAAPI, as textures that
// are its planes where they lie. The layer sits inside the surface, fills it, and passes the
// mouse and keys on to it.
//
// It is a window of its own inside the surface's window, not a widget. A widget that draws
// with OpenGL makes Qt rebuild the whole window it is put into, visibly, and from then on
// compose all of that window through OpenGL for every frame of video.
class VideoSurface::GlLayer : public QOpenGLWindow, protected QOpenGLFunctions
{
public:
    GlLayer(VideoSurface *owner, std::shared_ptr<Sink> sink)
        : QOpenGLWindow(sharedContext()), owner(owner), sink(std::move(sink))
    {
        // Several of these draw from one thread; waiting for the screen in each would have
        // them hold one another up.
        QSurfaceFormat format = requestedFormat();
        format.setSwapInterval(0);
        setFormat(format);
    }

    ~GlLayer() override
    {
        closeCard();
        if (glReady && context() && handle()) {
            makeCurrent();
            releaseGl();
            doneCurrent();
        }
    }

    // The overlays have changed and need painting again.
    void invalidateOverlays()
    {
        overlaysStale = true;
    }

    [[nodiscard]] bool drawsFromCard() const { return shown == Shown::OnCard; }

    // What is on the surface now, drawn afresh. Null before the window has been shown.
    [[nodiscard]] QImage snapshot()
    {
        if (!isValid() || !handle())
            return {};
        makeCurrent();
        paintGL();
        const QSize area = size() * devicePixelRatio();
        QImage image(area, QImage::Format_RGBA8888);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, area.width(), area.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
        // OpenGL counts rows from the bottom.
        image = image.mirrored();
        image.setDevicePixelRatio(devicePixelRatio());
        return image;
    }

protected:
    // Every window of this kind shares its textures with the others, so that one set of
    // hand-over workers serves them all.
    static QOpenGLContext *sharedContext()
    {
        static QOpenGLContext *const root = [] {
            auto *context = new QOpenGLContext(qApp);
            context->create();
            return context;
        }();
        return root->isValid() ? root : nullptr;
    }

    bool event(QEvent *event) override
    {
        // The last moment at which the context can be made current on this window.
        if (event->type() == QEvent::PlatformSurface && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && glReady) {
            makeCurrent();
            releaseGl();
            doneCurrent();
        }
        return QOpenGLWindow::event(event);
    }

    // Mouse and keys land on this window, but belong to the surface it is in.
    void mousePressEvent(QMouseEvent *event) override { QCoreApplication::sendEvent(owner, event); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { QCoreApplication::sendEvent(owner, event); }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QCoreApplication::sendEvent(owner, event);
        if (event->button() == Qt::RightButton) {
            QContextMenuEvent menu(QContextMenuEvent::Mouse, event->pos(), event->globalPos());
            QCoreApplication::sendEvent(owner, &menu);
        }
    }
    void keyPressEvent(QKeyEvent *event) override { QCoreApplication::sendEvent(owner->window(), event); }
    void focusInEvent(QFocusEvent *event) override
    {
        QOpenGLWindow::focusInEvent(event);
        // A click here brings the surface's window forward, and that is all it should do:
        // whatever was being typed into keeps the keyboard.
        if (QWindow *top = owner->window()->windowHandle())
            top->requestActivate();
    }

    // A new context: whatever was made in the last one went with it.
    void initializeGL() override
    {
        forgetGl();
    }

    void setupGl()
    {
        initializeOpenGLFunctions();
        program = std::make_unique<QOpenGLShaderProgram>();
        plainProgram = std::make_unique<QOpenGLShaderProgram>();
        glReady = true;

        // Written for OpenGL ES 2 and the desktop versions it is a subset of, so that the same
        // source runs through ANGLE and on old drivers.
        static const char *const VERTEX = R"(
            attribute vec2 position;
            attribute vec2 texcoord;
            varying vec2 coord;
            void main() {
                coord = texcoord;
                gl_Position = vec4(position, 0.0, 1.0);
            }
        )";
        static const char *const FRAGMENT = R"(
            #ifdef GL_ES
            precision mediump float;
            #endif
            varying vec2 coord;
            uniform sampler2D planeY;
            uniform sampler2D planeU;
            uniform sampler2D planeV;
            uniform float interleaved;
            uniform float pairInGreen;
            uniform vec2 lumaScale;
            uniform vec2 chromaScale;
            uniform mat3 conversion;
            uniform vec3 offset;
            void main() {
                float y = texture2D(planeY, coord * lumaScale).r;
                vec2 at = coord * chromaScale;
                vec2 chroma = interleaved > 0.5 ? texture2D(planeU, at).ra : vec2(texture2D(planeU, at).r, texture2D(planeV, at).r);
                if (pairInGreen > 0.5)
                    chroma = texture2D(planeU, at).rg;
                gl_FragColor = vec4(conversion * (vec3(y, chroma) - offset), 1.0);
            }
        )";

        usable = program->addShaderFromSourceCode(QOpenGLShader::Vertex, VERTEX) && program->addShaderFromSourceCode(QOpenGLShader::Fragment, FRAGMENT);
        program->bindAttributeLocation("position", 0);
        program->bindAttributeLocation("texcoord", 1);
        usable = usable && program->link();

        // TEMPORARY (Windows 7 bring-up).
        static std::once_flag describedGl;
        std::call_once(describedGl, [this] {
            const auto text = [this](GLenum name) { return QString::fromLatin1(reinterpret_cast<const char *>(glGetString(name))); };
            qCInfo(LogVideo).noquote() << "[hwdiag] OpenGL for video:" << text(GL_VENDOR) << "|" << text(GL_RENDERER) << "|" << text(GL_VERSION) << "| GLSL" << text(GL_SHADING_LANGUAGE_VERSION) << "| ES" << context()->isOpenGLES() << "| YUV shader" << (usable ? "works" : "FAILED: " + program->log());
#ifdef Q_OS_WIN
            qCInfo(LogVideo).noquote() << "[hwdiag] pictures on the card drawn directly:" << (QSettings().value(DIRECT_FRAMES_KEY, true).toBool() ? "allowed" : "turned off") << ", WGL_NV_DX_interop2" << (D3D11GlBridge::contextSupportsIt() ? "present" : "absent (pictures are read back and uploaded)");
#endif
        });

        // A texture that is already RGB: the overlays, and a picture made on the card.
        static const char *const PLAIN_FRAGMENT = R"(
            #ifdef GL_ES
            precision mediump float;
            #endif
            varying vec2 coord;
            uniform sampler2D image;
            uniform float opaque;
            void main() {
                vec4 colour = texture2D(image, coord);
                gl_FragColor = opaque > 0.5 ? vec4(colour.rgb, 1.0) : colour;
            }
        )";
        plainProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, VERTEX);
        plainProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, PLAIN_FRAGMENT);
        plainProgram->bindAttributeLocation("position", 0);
        plainProgram->bindAttributeLocation("texcoord", 1);
        plainProgram->link();

        glGenTextures(1, &overlayTexture);
        glBindTexture(GL_TEXTURE_2D, overlayTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (const GLuint texture : textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

#ifdef Q_OS_WIN
        if (usable && plainProgram->isLinked() && QSettings().value(DIRECT_FRAMES_KEY, true).toBool() && D3D11GlBridge::contextSupportsIt()) {
            if (auto worker = CardWorker::sharingWith(context())) {
                card = std::make_shared<CardPicture>(std::move(worker));
                sink->setCard(card);
            }
            // Making the worker's context can leave this one no longer current.
            makeCurrent();
        }
#elif defined(ACHERON_HAVE_DMABUF)
        if (usable && QSettings().value(DIRECT_FRAMES_KEY, true).toBool() && DmabufGlBridge::contextSupportsIt()) {
            card = std::make_unique<DmabufGlBridge>();
            sink->setCardUsable(true);
        }
#endif
    }

    void paintGL() override
    {
        if (!glReady)
            setupGl();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        QSize picture;
        bool onCard = false;
        sink->withFrame([&](const AVFrame *frame, quint64 serial, QSize displaySize) {
            if (frame->width <= 0 || !usable) {
                shown = Shown::Nothing;
                return;
            }
            picture = displaySize;
#ifdef ACHERON_HAVE_DMABUF
            if (frame->hw_frames_ctx && (shown != Shown::OnCard || serial != preparedSerial))
                takeFromCard(frame, serial);
#endif
            if (frame->hw_frames_ctx) {
                onCard = true;
            } else if (shown != Shown::Uploaded || serial != preparedSerial) {
                upload(frame);
                shown = Shown::Uploaded;
                preparedSerial = serial;
            }
        });
        updateFromCard(onCard);

        if (shown == Shown::Uploaded) {
            draw(picture);
        } else if (shown == Shown::OnCard) {
#ifdef ACHERON_HAVE_DMABUF
            draw(picture);
#else
            placePicture(picture);
            drawTexture(cardTexture, true);
#endif
        }
        drawOverlays(shown != Shown::Nothing);
        if (owner->speaking)
            drawSpeakingRing();
    }

private:
    enum class Shown {
        Nothing,
        Uploaded,
        OnCard,
    };

    // Takes the newest finished picture from the card when the frame to show is one of its.
    // Until the first is finished, whatever was showing stays up.
    void updateFromCard(bool onCard)
    {
#ifdef Q_OS_WIN
        if (onCard) {
            cardTexture = card ? card->acquire() : 0;
            if (cardTexture)
                shown = Shown::OnCard;
            else if (shown == Shown::OnCard)
                shown = Shown::Nothing;
        } else if (card && shown == Shown::Nothing) {
            // An old picture is not to come back when the next one is still on its way.
            card->discard();
        }
#else
        Q_UNUSED(onCard);
#endif
#ifdef ACHERON_HAVE_DMABUF
        if (card && shown != Shown::OnCard)
            card->release();
#endif
    }

#ifdef ACHERON_HAVE_DMABUF
    // Within withFrame(). A card that will not have the picture is not offered another, and
    // the picture is then in memory, to be uploaded.
    void takeFromCard(const AVFrame *frame, quint64 serial)
    {
        const AVFrame *exported = sink->exportedFrame();
        if (!card || !exported || !card->update(exported, textures[FIRST_CARD_TEXTURE], textures[FIRST_CARD_TEXTURE + 1])) {
            closeCard();
            sink->readBack();
            return;
        }
        nv12 = true;
        // The surface is as large as the coded picture, which is rounded up to whole blocks.
        // As with rows in memory, the shader is told how much of it to read.
        lumaScale = QVector2D((static_cast<float>(frame->width) - 0.5f) / static_cast<float>(exported->width), (static_cast<float>(frame->height) - 0.5f) / static_cast<float>(exported->height));
        chromaScale = QVector2D((static_cast<float>((frame->width + 1) / 2) - 0.5f) / static_cast<float>((exported->width + 1) / 2), (static_cast<float>((frame->height + 1) / 2) - 0.5f) / static_cast<float>((exported->height + 1) / 2));
        setConversion(frame);
        shown = Shown::OnCard;
        preparedSerial = serial;
    }
#endif

    // Everything made in the context, which is current and about to go. The frame itself is
    // with the sink, so the picture comes back in the next context.
    void releaseGl()
    {
        // The card's pictures are in textures of this context's share group.
        closeCard();
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
        glDeleteTextures(1, &overlayTexture);
        forgetGl();
    }

    void forgetGl()
    {
        closeCard();
        textures = {};
        overlayTexture = 0;
        allocations = {};
        overlaySize = {};
        overlaysStale = true;
        program.reset();
        plainProgram.reset();
        usable = false;
        glReady = false;
        shown = Shown::Nothing;
    }

    void closeCard()
    {
#ifdef Q_OS_WIN
        if (card) {
            sink->setCard(nullptr);
            card->close();
            card.reset();
        }
#elif defined(ACHERON_HAVE_DMABUF)
        if (card) {
            sink->setCardUsable(false);
            card.reset();
        }
#endif
        cardTexture = 0;
        if (shown == Shown::OnCard)
            shown = Shown::Nothing;
    }

    // Sets the viewport to where the picture belongs: as large as fits, in the middle.
    void placePicture(QSize picture)
    {
        const QSize area = size() * devicePixelRatio();
        const QSize where = fitted(picture, area);
        glViewport((area.width() - where.width()) / 2, (area.height() - where.height()) / 2, where.width(), where.height());
    }

    // One RGB texture over the whole viewport.
    void drawTexture(GLuint texture, bool opaque)
    {
        static const GLfloat POSITIONS[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        static const GLfloat TEXCOORDS[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

        plainProgram->bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        plainProgram->setUniformValue("image", 0);
        plainProgram->setUniformValue("opaque", opaque ? 1.0f : 0.0f);
        // The arrays are in memory, which only means that with no buffer bound.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        plainProgram->enableAttributeArray(0);
        plainProgram->enableAttributeArray(1);
        plainProgram->setAttributeArray(0, GL_FLOAT, POSITIONS, 2);
        plainProgram->setAttributeArray(1, GL_FLOAT, TEXCOORDS, 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        plainProgram->disableAttributeArray(0);
        plainProgram->disableAttributeArray(1);
        glBindTexture(GL_TEXTURE_2D, 0);
        plainProgram->release();
    }

    // The overlays change a few times a minute and the picture sixty times a second, so they
    // are painted into an image only when they change, kept as a texture, and laid over each
    // frame with one blended quad.
    void drawOverlays(bool hasPicture)
    {
        const qreal ratio = devicePixelRatio();
        const QSize area = size() * ratio;
        if (area.isEmpty() || !plainProgram || !plainProgram->isLinked())
            return;

        if (!owner->hasOverlaysBesideTheRing(hasPicture))
            return;

        if (overlaysStale || hasPicture != overlaysHadPicture || area != overlaySize) {
            QImage image(area, QImage::Format_RGBA8888_Premultiplied);
            image.setDevicePixelRatio(ratio);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                owner->paintOverlays(painter, hasPicture, false);
            }

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, overlayTexture);
            if (area != overlaySize)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, area.width(), area.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
            else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, area.width(), area.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());

            overlaysStale = false;
            overlaysHadPicture = hasPicture;
            overlaySize = area;
        }

        glViewport(0, 0, area.width(), area.height());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        drawTexture(overlayTexture, false);
        glDisable(GL_BLEND);
    }

    // The ring goes on and off with every word, which is too often to paint everything over
    // the picture again for. Two pixels along each edge, as the painter draws it.
    void drawSpeakingRing()
    {
        const QSize area = size() * devicePixelRatio();
        const int thickness = qMax(1, qRound(2 * devicePixelRatio()));
        glEnable(GL_SCISSOR_TEST);
        glClearColor(static_cast<GLfloat>(SPEAKING_RING.redF()), static_cast<GLfloat>(SPEAKING_RING.greenF()), static_cast<GLfloat>(SPEAKING_RING.blueF()), 1.0f);
        for (const QRect &edge : { QRect(0, 0, area.width(), thickness), QRect(0, area.height() - thickness, area.width(), thickness), QRect(0, 0, thickness, area.height()), QRect(area.width() - thickness, 0, thickness, area.height()) }) {
            glScissor(edge.x(), edge.y(), edge.width(), edge.height());
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    void uploadPlane(int unit, GLenum format, int textureWidth, int textureHeight, const uint8_t *data)
    {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, textures[static_cast<size_t>(unit)]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        Allocation &allocation = allocations[static_cast<size_t>(unit)];
        if (allocation.format != format || allocation.size != QSize(textureWidth, textureHeight)) {
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), textureWidth, textureHeight, 0, format, GL_UNSIGNED_BYTE, data);
            allocation = { format, QSize(textureWidth, textureHeight) };
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, format, GL_UNSIGNED_BYTE, data);
        }
    }

    void upload(const AVFrame *frame)
    {
        const int chromaWidth = (frame->width + 1) / 2;
        const int chromaHeight = (frame->height + 1) / 2;
        nv12 = frame->format == AV_PIX_FMT_NV12;

        // A row in memory is wider than the picture; the whole row goes up, and the shader is
        // told how much of it to read. Half a texel is held back so that filtering at the edge
        // does not reach into the padding.
        uploadPlane(0, GL_LUMINANCE, frame->linesize[0], frame->height, frame->data[0]);
        lumaScale = QVector2D((static_cast<float>(frame->width) - 0.5f) / static_cast<float>(frame->linesize[0]), 1.0f);

        if (nv12) {
            const int pairs = frame->linesize[1] / 2;
            uploadPlane(1, GL_LUMINANCE_ALPHA, pairs, chromaHeight, frame->data[1]);
            chromaScale = QVector2D((static_cast<float>(chromaWidth) - 0.5f) / static_cast<float>(pairs), 1.0f);
        } else {
            uploadPlane(1, GL_LUMINANCE, frame->linesize[1], chromaHeight, frame->data[1]);
            uploadPlane(2, GL_LUMINANCE, frame->linesize[2], chromaHeight, frame->data[2]);
            chromaScale = QVector2D((static_cast<float>(chromaWidth) - 0.5f) / static_cast<float>(frame->linesize[1]), 1.0f);
        }
        glActiveTexture(GL_TEXTURE0);
        // Back to the default, which the overlay texture is uploaded with.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        setConversion(frame);
    }

    void setConversion(const AVFrame *frame)
    {
        // Y'CbCr to RGB for the frame's standard and range.
        const bool bt709 = usesBt709(frame);
        const float kr = bt709 ? 0.2126f : 0.299f;
        const float kb = bt709 ? 0.0722f : 0.114f;
        const float kg = 1.0f - kr - kb;
        const bool full = isFullRange(frame);
        const float lumaGain = full ? 1.0f : 255.0f / 219.0f;
        const float chromaGain = full ? 1.0f : 255.0f / 224.0f;
        const float values[9] = {
            lumaGain, 0.0f, (2.0f - 2.0f * kr) * chromaGain,
            lumaGain, -(2.0f * kb * (1.0f - kb) / kg) * chromaGain, -(2.0f * kr * (1.0f - kr) / kg) * chromaGain,
            lumaGain, (2.0f - 2.0f * kb) * chromaGain, 0.0f
        };
        conversion = QMatrix3x3(values);
        offset = QVector3D(full ? 0.0f : 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
    }

    void draw(QSize picture)
    {
        placePicture(picture);

        static const GLfloat POSITIONS[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        static const GLfloat TEXCOORDS[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

        program->bind();
        const size_t first = shown == Shown::OnCard ? FIRST_CARD_TEXTURE : 0;
        for (int unit = 0; unit < 3; unit++) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glBindTexture(GL_TEXTURE_2D, textures[first + static_cast<size_t>(unit)]);
        }
        program->setUniformValue("planeY", 0);
        program->setUniformValue("planeU", 1);
        program->setUniformValue("planeV", 2);
        program->setUniformValue("interleaved", nv12 ? 1.0f : 0.0f);
        program->setUniformValue("pairInGreen", shown == Shown::OnCard ? 1.0f : 0.0f);
        program->setUniformValue("lumaScale", lumaScale);
        program->setUniformValue("chromaScale", chromaScale);
        program->setUniformValue("conversion", conversion);
        program->setUniformValue("offset", offset);

        // The arrays below are in memory, which only means that with no buffer bound.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        program->enableAttributeArray(0);
        program->enableAttributeArray(1);
        program->setAttributeArray(0, GL_FLOAT, POSITIONS, 2);
        program->setAttributeArray(1, GL_FLOAT, TEXCOORDS, 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        program->disableAttributeArray(0);
        program->disableAttributeArray(1);

        for (int unit = 2; unit >= 0; unit--) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        program->release();
    }

    struct Allocation
    {
        GLenum format = 0;
        QSize size;
    };

    VideoSurface *owner;
    std::shared_ptr<Sink> sink;
    std::unique_ptr<QOpenGLShaderProgram> program;
    std::unique_ptr<QOpenGLShaderProgram> plainProgram;
    GLuint overlayTexture = 0;
    QSize overlaySize;
    bool overlaysStale = true;
    bool overlaysHadPicture = false;
    // The planes of a frame uploaded, and then those of one on the card.
    static constexpr size_t FIRST_CARD_TEXTURE = 3;
    std::array<GLuint, 6> textures = {};
    std::array<Allocation, 3> allocations;
#ifdef Q_OS_WIN
    std::shared_ptr<CardPicture> card;
#elif defined(ACHERON_HAVE_DMABUF)
    std::unique_ptr<DmabufGlBridge> card;
#endif
    bool glReady = false;
    GLuint cardTexture = 0;
    Shown shown = Shown::Nothing;
    quint64 preparedSerial = 0;
    bool usable = false;
    bool nv12 = false;
    QVector2D lumaScale;
    QVector2D chromaScale;
    QMatrix3x3 conversion;
    QVector3D offset;
};

bool VideoSurface::gpuDrawingEnabled()
{
    return QSettings().value(GPU_DRAWING_KEY, true).toBool();
}

void VideoSurface::setGpuDrawingEnabled(bool enabled)
{
    QSettings().setValue(GPU_DRAWING_KEY, enabled);
}

VideoSurface::VideoSurface(QWidget *parent)
    : QWidget(parent), usesGpu(gpuDrawingEnabled() && openGlWorks())
{
    frameSink = std::make_shared<Sink>(this, usesGpu);

    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);

    rescaleTimer.setSingleShot(true);
    rescaleTimer.setInterval(RESCALE_DELAY_MS);
    connect(&rescaleTimer, &QTimer::timeout, this, [this] {
        frameSink->rescale();
        refresh();
    });

    notShownTimer.setSingleShot(true);
    notShownTimer.setInterval(NOT_SHOWN_BEFORE_RELEASED_MS);
    connect(&notShownTimer, &QTimer::timeout, this, &VideoSurface::releaseWhatIsNotShown);
}

void VideoSurface::makeLayerForAPicture()
{
    if (glLayer || !usesGpu || !isVisible() || !frameSink->hasPicture())
        return;
    glLayer = new GlLayer(this, frameSink);
    // The container owns the layer's window and keeps it over this widget.
    glContainer = QWidget::createWindowContainer(glLayer, this);
    glContainer->setFocusPolicy(Qt::NoFocus);
    glContainer->setAttribute(Qt::WA_TransparentForMouseEvents);
    glContainer->setGeometry(rect());
    glContainer->show();
}

void VideoSurface::dropLayer()
{
    if (!glContainer)
        return;
    delete glContainer;
    glContainer = nullptr;
    glLayer = nullptr;
}

void VideoSurface::releaseWhatIsNotShown()
{
    if (isVisible())
        return;
    dropLayer();
    frameSink->keepInMemory();
}

VideoSurface::~VideoSurface()
{
    frameSink->detach();
    // Now, while the rest of this is still here for it.
    delete glContainer;
}

QImage VideoSurface::snapshot()
{
    return glLayer ? glLayer->snapshot() : grab().toImage();
}

std::shared_ptr<Core::Media::VideoFrameSink> VideoSurface::sink() const
{
    return frameSink;
}

bool VideoSurface::drawsFromCard() const
{
    return glLayer && glLayer->drawsFromCard();
}

void VideoSurface::refresh()
{
    if (glLayer)
        glLayer->update();
    else
        update();
}

// For a change to what is drawn over the picture, as opposed to the picture.
void VideoSurface::refreshOverlays()
{
    if (glLayer)
        glLayer->invalidateOverlays();
    refresh();
}

void VideoSurface::setMessage(const QString &title, const QString &detail)
{
    if (title == messageTitle && detail == messageDetail)
        return;
    messageTitle = title;
    messageDetail = detail;
    refreshOverlays();
}

void VideoSurface::setStatsText(const QString &text)
{
    if (text == statsText)
        return;
    statsText = text;
    refreshOverlays();
}

void VideoSurface::setCaption(const QString &text)
{
    if (text == captionText)
        return;
    captionText = text;
    refreshOverlays();
}

void VideoSurface::setPlaceholder(const QPixmap &avatar)
{
    if (avatar.cacheKey() == placeholder.cacheKey())
        return;
    placeholder = avatar;
    refreshOverlays();
}

void VideoSurface::setSpeaking(bool value)
{
    if (speaking == value)
        return;
    speaking = value;
    refresh();
}

void VideoSurface::announceGeometry()
{
    reportGeometry(true);
}

void VideoSurface::clear()
{
    frameSink->forget();
    dropLayer();
    refresh();
}

void VideoSurface::onFrameArrived()
{
    makeLayerForAPicture();
    // Not while pictures still come for it, which they do for a while after it is hidden.
    if (notShownTimer.isActive())
        notShownTimer.start();
    refresh();
    reportGeometry();
}

void VideoSurface::reportGeometry(bool evenIfUnchanged)
{
    const QSize picture = frameSink->pictureSize();
    // Before the first frame the picture's shape is unknown, so the whole widget stands in.
    const QSize area = picture.isEmpty() ? size() * devicePixelRatioF() : picture;
    const int pixels = area.width() * area.height();
    const bool visible = isVisible() && !window()->isMinimized();
    // This runs for every frame, and almost never has anything new to say.
    if (!evenIfUnchanged && pixels == reportedPixels && visible == reportedVisible)
        return;
    reportedPixels = pixels;
    reportedVisible = visible;
    emit geometryChanged(pixels, visible);
}

void VideoSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (glContainer)
        glContainer->setGeometry(rect());
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    rescaleTimer.start();
    reportGeometry();
}

void VideoSurface::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    notShownTimer.stop();
    frameSink->setShown(true);
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    makeLayerForAPicture();
    reportGeometry();
}

void VideoSurface::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    frameSink->setShown(false);
    notShownTimer.start();
    reportedPixels = 0;
    reportedVisible = false;
    emit geometryChanged(0, false);
}

void VideoSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    QWidget::mouseDoubleClickEvent(event);
    emit doubleClicked();
}

void VideoSurface::mouseReleaseEvent(QMouseEvent *event)
{
    QWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit clicked();
}

void VideoSurface::paintEvent(QPaintEvent *)
{
    // With a layer for the graphics card, the layer covers this widget and draws everything.
    if (glLayer)
        return;

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    bool hasPicture = false;
    frameSink->withFront([&](const QImage &image) {
        if (image.isNull())
            return;
        hasPicture = true;
        const qreal ratio = devicePixelRatioF();
        const QSize widgetPixels = size() * ratio;
        if (image.size() == fitted(image.size(), widgetPixels)) {
            // Already at the right size: place it on whole device pixels so it is copied, not resampled.
            const QPoint offset((widgetPixels.width() - image.width()) / 2, (widgetPixels.height() - image.height()) / 2);
            painter.drawImage(QPointF(offset) / ratio, image);
            return;
        }
        // While a resize is in progress the picture is still at its old size.
        const QSizeF shown = QSizeF(image.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF where(QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0), shown);
        painter.drawImage(where, image, QRectF(QPointF(0, 0), QSizeF(image.size())));
    });

    paintOverlays(painter, hasPicture, true);
}

bool VideoSurface::hasOverlaysBesideTheRing(bool hasPicture) const
{
    return (!hasPicture && !placeholder.isNull()) || !messageTitle.isEmpty() || !captionText.isEmpty() || !statsText.isEmpty();
}

void VideoSurface::paintOverlays(QPainter &painter, bool hasPicture, bool withSpeakingRing)
{
    if (!hasPicture && !placeholder.isNull()) {
        const int side = qBound(24, qMin(width(), height()) / 3, 128);
        const QRect where((width() - side) / 2, (height() - side) / 2, side, side);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(where, placeholder);
    }

    if (!messageTitle.isEmpty()) {
        painter.fillRect(rect(), QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);

        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
        painter.setFont(titleFont);
        const QRect titleArea = rect().adjusted(16, 0, -16, -rect().height() / 2);
        painter.drawText(titleArea, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, messageTitle);

        painter.setFont(font());
        const QRect detailArea = rect().adjusted(16, rect().height() / 2 + 6, -16, 0);
        painter.drawText(detailArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, messageDetail);
    }

    if (!captionText.isEmpty()) {
        // Measured with the widget's metrics: the painter may be on the OpenGL framebuffer,
        // whose own idea of a font's size is not to be relied on.
        painter.setFont(font());
        const QRect bounds = fontMetrics().boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignBottom, captionText);
        painter.fillRect(bounds.adjusted(-4, -2, 4, 2), QColor(0, 0, 0, 170));
        painter.setPen(Qt::white);
        painter.drawText(bounds, Qt::AlignLeft | Qt::AlignBottom, captionText);
    }

    if (speaking && withSpeakingRing) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(SPEAKING_RING, 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }

    if (!statsText.isEmpty()) {
        const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        painter.setFont(fixed);
        const QRect bounds = QFontMetrics(fixed, this).boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop, statsText);
        // Left off a tile too small to hold them and still show its picture.
        if (bounds.height() <= height() / 2 && bounds.width() <= width() - 16) {
            painter.fillRect(bounds.adjusted(-4, -4, 4, 4), QColor(0, 0, 0, 170));
            painter.setPen(Qt::white);
            painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop, statsText);
        }
    }
}

} // namespace UI
} // namespace Acheron
