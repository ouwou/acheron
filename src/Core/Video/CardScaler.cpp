#include "Core/Video/CardScaler.hpp"

#include "Core/Logging.hpp"

#include <QString>

#include <atomic>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#ifdef Q_OS_WIN
#include <windows.h>

#include <d3d11_1.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#elif defined(ACHERON_HAVE_AVFILTER)
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}
#endif

namespace Acheron {
namespace Core {
namespace Video {

#if defined(Q_OS_WIN) || defined(ACHERON_HAVE_AVFILTER)

namespace {

// Whether the card has scaled a picture, or refused to in a way it will go on refusing.
enum class Verdict {
    Untried,
    Works,
    RuledOut,
};
std::atomic<Verdict> verdict{ Verdict::Untried };

} // namespace

#endif

#ifdef Q_OS_WIN

namespace {

// A window that moves from screen to screen brings a size of texture with every screen.
constexpr size_t MOST_PROCESSORS = 4;

template <typename T>
void drop(T *&object)
{
    if (object)
        object->Release();
    object = nullptr;
}

// The card's one context for drawing is shared with whatever captures and with the encoder.
class DeviceLock
{
public:
    explicit DeviceLock(AVD3D11VADeviceContext *device) : device(device) { device->lock(device->lock_ctx); }
    ~DeviceLock() { device->unlock(device->lock_ctx); }

    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;

private:
    AVD3D11VADeviceContext *device;
};

AVD3D11VADeviceContext *direct3d(AVBufferRef *device)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
}

RECT native(const QRect &rect)
{
    return { rect.left(), rect.top(), rect.left() + rect.width(), rect.top() + rect.height() };
}

// From the screen's full-range RGB to what the pictures are marked as. Left alone, the
// processor converts with coefficients of the driver's choosing.
void setColourSpaces(ID3D11VideoContext *context, ID3D11VideoProcessor *processor, bool toYuv, bool bt601)
{
    // The older call leaves the meaning of the RGB range to the driver, and drivers disagree.
    ID3D11VideoContext1 *modern = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&modern)))) {
        DXGI_COLOR_SPACE_TYPE output = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (toYuv)
            output = bt601 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        modern->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        modern->VideoProcessorSetOutputColorSpace1(processor, output);
        modern->Release();
        return;
    }

    // All zero is full-range RGB.
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE input = {};
    context->VideoProcessorSetStreamColorSpace(processor, 0, &input);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output = {};
    if (toYuv) {
        output.YCbCr_Matrix = bt601 ? 0 : 1;
        output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    }
    context->VideoProcessorSetOutputColorSpace(processor, &output);
}

} // namespace

struct CardScaler::Card
{
    // One for every size of texture pictures have come in. A window comes cut out of its
    // screen's picture and in one of its own by turns.
    struct Processor
    {
        QSize textureSize;
        DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
        ID3D11VideoProcessorEnumerator *enumerator = nullptr;
        ID3D11VideoProcessor *processor = nullptr;
    };

    ~Card() { release(); }

    bool scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &placed, AVFrame *into);
    const Processor *processorFor(ID3D11Texture2D *texture);
    bool choosePictures(const Processor &processor);
    AVBufferRef *picturesOf(AVPixelFormat format, UINT bindFlags, const Processor &processor);
    void setUp(const Processor &processor);
    void release();

    AVBufferRef *device = nullptr;
    Output output;
    // The frames the scaled pictures are, which an encoder is opened with: they stay the same
    // for as long as the card and the output do.
    AVBufferRef *pictures = nullptr;
    AVPixelFormat pictureFormat = AV_PIX_FMT_NONE;
    std::vector<Processor> processors;
    // What the card would not do, for the log.
    const char *refused = "";
    HRESULT refusedWith = S_OK;
};

void CardScaler::Card::release()
{
    for (Processor &each : processors) {
        drop(each.processor);
        drop(each.enumerator);
    }
    processors.clear();
    av_buffer_unref(&pictures);
    av_buffer_unref(&device);
    pictureFormat = AV_PIX_FMT_NONE;
}

bool CardScaler::Card::scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &placed, AVFrame *into)
{
    const auto *source = reinterpret_cast<const AVHWFramesContext *>(from->hw_frames_ctx->data);
    if (!device || device->data != source->device_ref->data || !(wanted == output)) {
        release();
        device = av_buffer_ref(source->device_ref);
        output = wanted;
    }
    refusedWith = S_OK;
    refused = "it has no video processor";
    AVD3D11VADeviceContext *d3d = device ? direct3d(device) : nullptr;
    if (!d3d || !d3d->video_device || !d3d->video_context)
        return false;

    auto *texture = reinterpret_cast<ID3D11Texture2D *>(from->data[0]);
    const Processor *processor = processorFor(texture);
    if (!processor)
        return false;

    av_frame_unref(into);
    refused = "it made no texture for a scaled picture";
    if (av_hwframe_get_buffer(pictures, into, 0) < 0)
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC sourceView = {};
    sourceView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    sourceView.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<quintptr>(from->data[1]));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC targetView = {};
    targetView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorInputView *input = nullptr;
    ID3D11VideoProcessorOutputView *target = nullptr;

    refused = "its video processor does not read the picture";
    refusedWith = d3d->video_device->CreateVideoProcessorInputView(texture, processor->enumerator, &sourceView, &input);
    if (SUCCEEDED(refusedWith)) {
        refused = "its video processor does not draw into the scaled picture";
        refusedWith = d3d->video_device->CreateVideoProcessorOutputView(reinterpret_cast<ID3D11Texture2D *>(into->data[0]), processor->enumerator, &targetView, &target);
    }
    if (SUCCEEDED(refusedWith)) {
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = input;
        const RECT cut = native(area & QRect(QPoint(0, 0), processor->textureSize));
        const RECT drawn = native(placed & QRect(QPoint(0, 0), output.size));

        refused = "its video processor would not draw";
        const DeviceLock lock(d3d);
        d3d->video_context->VideoProcessorSetStreamSourceRect(processor->processor, 0, TRUE, &cut);
        d3d->video_context->VideoProcessorSetStreamDestRect(processor->processor, 0, TRUE, &drawn);
        refusedWith = d3d->video_context->VideoProcessorBlt(processor->processor, target, 0, 1, &stream);
    }
    drop(input);
    drop(target);
    if (FAILED(refusedWith)) {
        av_frame_unref(into);
        return false;
    }

    into->colorspace = pictureFormat == AV_PIX_FMT_NV12 && output.bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    into->color_range = AVCOL_RANGE_MPEG;
    return true;
}

const CardScaler::Card::Processor *CardScaler::Card::processorFor(ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC description;
    texture->GetDesc(&description);
    const QSize textureSize(static_cast<int>(description.Width), static_cast<int>(description.Height));
    for (const Processor &each : processors) {
        if (each.textureSize == textureSize && each.textureFormat == description.Format)
            return &each;
    }
    if (processors.size() >= MOST_PROCESSORS) {
        for (Processor &each : processors) {
            drop(each.processor);
            drop(each.enumerator);
        }
        processors.clear();
    }

    Processor made;
    made.textureSize = textureSize;
    made.textureFormat = description.Format;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = { 60, 1 };
    content.InputWidth = description.Width;
    content.InputHeight = description.Height;
    content.OutputFrameRate = { 60, 1 };
    content.OutputWidth = static_cast<UINT>(output.size.width());
    content.OutputHeight = static_cast<UINT>(output.size.height());
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ID3D11VideoDevice *video = direct3d(device)->video_device;
    refused = "it made no video processor";
    refusedWith = video->CreateVideoProcessorEnumerator(&content, &made.enumerator);
    if (SUCCEEDED(refusedWith)) {
        UINT support = 0;
        refused = "its video processor does not take the format of the picture";
        refusedWith = made.enumerator->CheckVideoProcessorFormat(description.Format, &support);
        if (SUCCEEDED(refusedWith) && !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
            refusedWith = E_NOTIMPL;
    }
    if (SUCCEEDED(refusedWith)) {
        refused = "it made no video processor";
        refusedWith = video->CreateVideoProcessor(made.enumerator, 0, &made.processor);
    }
    if (FAILED(refusedWith) || (!pictures && !choosePictures(made))) {
        drop(made.processor);
        drop(made.enumerator);
        return nullptr;
    }

    setUp(made);
    processors.push_back(made);
    return &processors.back();
}

bool CardScaler::Card::choosePictures(const Processor &processor)
{
    refused = "its video processor makes no pictures an encoder takes";
    refusedWith = S_OK;
    for (AVPixelFormat format : { AV_PIX_FMT_NV12, AV_PIX_FMT_BGRA }) {
        if (format == AV_PIX_FMT_BGRA && !output.rgbWillDo)
            break;
        UINT support = 0;
        const DXGI_FORMAT made = format == AV_PIX_FMT_NV12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(processor.enumerator->CheckVideoProcessorFormat(made, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            continue;

        // Not every driver makes a texture that is drawn into and is an encoder's input as well.
        for (UINT bindFlags : { static_cast<UINT>(D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER), static_cast<UINT>(D3D11_BIND_RENDER_TARGET) }) {
            pictures = picturesOf(format, bindFlags, processor);
            if (pictures) {
                pictureFormat = format;
                return true;
            }
        }
    }
    return false;
}

// Frames for the scaled pictures, once the card has made one and the processor taken it as
// something to draw into. Null otherwise.
AVBufferRef *CardScaler::Card::picturesOf(AVPixelFormat format, UINT bindFlags, const Processor &processor)
{
    AVBufferRef *made = av_hwframe_ctx_alloc(device);
    if (!made)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(made->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = format;
    frames->width = output.size.width();
    frames->height = output.size.height();
    // None to begin with has ffmpeg make every texture by itself as it is asked for. Any other
    // number is that many in one texture array, which is what a Radeon will not make of NV12
    // render targets.
    frames->initial_pool_size = 0;
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = bindFlags;

    AVFrame *first = av_frame_alloc();
    bool usable = av_hwframe_ctx_init(made) >= 0 && av_hwframe_get_buffer(made, first, 0) >= 0;
    if (usable) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description = {};
        description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ID3D11VideoProcessorOutputView *view = nullptr;
        usable = SUCCEEDED(direct3d(device)->video_device->CreateVideoProcessorOutputView(reinterpret_cast<ID3D11Texture2D *>(first->data[0]), processor.enumerator, &description, &view));
        drop(view);
    }
    av_frame_free(&first);
    if (!usable)
        av_buffer_unref(&made);
    return made;
}

void CardScaler::Card::setUp(const Processor &processor)
{
    AVD3D11VADeviceContext *d3d = direct3d(device);
    ID3D11VideoContext *context = d3d->video_context;
    const RECT whole = native(QRect(QPoint(0, 0), output.size));
    D3D11_VIDEO_COLOR black = {};
    black.RGBA.A = 1;

    const DeviceLock lock(d3d);
    context->VideoProcessorSetStreamFrameFormat(processor.processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // No sharpening, noise reduction or the like, which drivers apply to what they take for film.
    context->VideoProcessorSetStreamAutoProcessingMode(processor.processor, 0, FALSE);
    context->VideoProcessorSetOutputTargetRect(processor.processor, TRUE, &whole);
    context->VideoProcessorSetOutputBackgroundColor(processor.processor, FALSE, &black);
    setColourSpaces(context, processor.processor, pictureFormat == AV_PIX_FMT_NV12, output.bt601);
}

CardScaler::CardScaler() : card(std::make_unique<Card>()) {}

CardScaler::~CardScaler() = default;

bool CardScaler::usable()
{
    return verdict != Verdict::RuledOut;
}

bool CardScaler::scale(const AVFrame *from, const QRect &area, const Output &output, const QRect &placed, AVFrame *into)
{
    if (from->format != AV_PIX_FMT_D3D11 || !from->hw_frames_ctx || output.size.isEmpty())
        return false;

    Verdict untried = Verdict::Untried;
    if (card->scale(from, area, output, placed, into)) {
        verdict.compare_exchange_strong(untried, Verdict::Works);
        return true;
    }

    const QString reason = QStringLiteral("%1 (%2)").arg(QLatin1String(card->refused), QString::number(static_cast<quint32>(card->refusedWith), 16));
    // A card that Windows has just taken away, as it does on a change of driver or of screen
    // mode, has said nothing of what it can do.
    auto *lent = static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWFramesContext *>(from->hw_frames_ctx->data)->device_ctx->hwctx);
    const bool cardTakenAway = FAILED(lent->device->GetDeviceRemovedReason());
    if (!cardTakenAway && verdict.compare_exchange_strong(untried, Verdict::RuledOut))
        qCInfo(LogVideo).noquote() << "The graphics card does not scale pictures:" << reason << "; they are read back and scaled in software";
    else if (cardTakenAway || verdict == Verdict::Works)
        qCWarning(LogVideo).noquote() << "The graphics card did not scale a picture:" << reason;
    card->release();
    return false;
}

#elif defined(ACHERON_HAVE_AVFILTER)

struct CardScaler::Card
{
    ~Card()
    {
        release();
        av_frame_free(&cut);
    }

    bool scale(const AVFrame *from, const QRect &area, const Output &wanted, AVFrame *into);
    bool build(const AVFrame *from);
    void release();

    AVBufferRef *device = nullptr;
    Output output;
    QSize sourceSize;
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr;
    AVFilterContext *sink = nullptr;
    AVFrame *cut = av_frame_alloc();
    // What the card would not do, for the log.
    const char *refused = "";
    int refusedWith = 0;
};

void CardScaler::Card::release()
{
    avfilter_graph_free(&graph);
    source = nullptr;
    sink = nullptr;
    av_buffer_unref(&device);
}

bool CardScaler::Card::scale(const AVFrame *from, const QRect &area, const Output &wanted, AVFrame *into)
{
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(from->hw_frames_ctx->data);
    const QSize size(from->width, from->height);
    if (!graph || device->data != frames->device_ref->data || !(wanted == output) || size != sourceSize || frames->sw_format != sourceFormat) {
        release();
        device = av_buffer_ref(frames->device_ref);
        output = wanted;
        sourceSize = size;
        sourceFormat = frames->sw_format;
        if (!build(from))
            return false;
    }

    const QRect taken = area & QRect(QPoint(0, 0), size);
    av_frame_unref(cut);
    refused = "it would not take the picture";
    refusedWith = av_frame_ref(cut, from);
    if (refusedWith < 0)
        return false;
    cut->crop_left = static_cast<size_t>(taken.left());
    cut->crop_top = static_cast<size_t>(taken.top());
    cut->crop_right = static_cast<size_t>(size.width() - taken.left() - taken.width());
    cut->crop_bottom = static_cast<size_t>(size.height() - taken.top() - taken.height());
    cut->color_range = AVCOL_RANGE_JPEG;

    refusedWith = av_buffersrc_add_frame_flags(source, cut, AV_BUFFERSRC_FLAG_PUSH | AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT);
    if (refusedWith < 0)
        return false;
    av_frame_unref(into);
    refused = "its video processor made no picture";
    refusedWith = av_buffersink_get_frame(sink, into);
    return refusedWith >= 0;
}

bool CardScaler::Card::build(const AVFrame *from)
{
    refused = "this ffmpeg has no scale_vaapi";
    refusedWith = 0;
    const AVFilter *scaler = avfilter_get_by_name("scale_vaapi");
    graph = scaler ? avfilter_graph_alloc() : nullptr;
    if (!graph)
        return false;

    refused = "it made no video processor";
    source = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
    AVBufferSrcParameters *parameters = source ? av_buffersrc_parameters_alloc() : nullptr;
    if (!parameters)
        return false;
    parameters->format = AV_PIX_FMT_VAAPI;
    parameters->width = from->width;
    parameters->height = from->height;
    parameters->time_base = { 1, 90000 };
    parameters->hw_frames_ctx = from->hw_frames_ctx;
    refusedWith = av_buffersrc_parameters_set(source, parameters);
    av_free(parameters);
    if (refusedWith >= 0)
        refusedWith = avfilter_init_dict(source, nullptr);

    const QByteArray settings = QStringLiteral("w=%1:h=%2:format=nv12:out_color_matrix=%3:out_range=limited").arg(output.size.width()).arg(output.size.height()).arg(QLatin1String(output.bt601 ? "smpte170m" : "bt709")).toLatin1();
    AVFilterContext *scale = nullptr;
    if (refusedWith >= 0)
        refusedWith = avfilter_graph_create_filter(&scale, scaler, "scale", settings.constData(), nullptr, graph);
    if (refusedWith >= 0)
        refusedWith = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph);
    if (refusedWith >= 0)
        refusedWith = avfilter_link(source, 0, scale, 0);
    if (refusedWith >= 0)
        refusedWith = avfilter_link(scale, 0, sink, 0);
    if (refusedWith >= 0)
        refusedWith = avfilter_graph_config(graph, nullptr);
    return refusedWith >= 0;
}

CardScaler::CardScaler() : card(std::make_unique<Card>()) {}

CardScaler::~CardScaler() = default;

bool CardScaler::usable()
{
    return verdict != Verdict::RuledOut;
}

bool CardScaler::scale(const AVFrame *from, const QRect &area, const Output &output, const QRect &placed, AVFrame *into)
{
    if (from->format != AV_PIX_FMT_VAAPI || !from->hw_frames_ctx || output.size.isEmpty() || placed != QRect(QPoint(0, 0), output.size))
        return false;

    Verdict untried = Verdict::Untried;
    if (card->scale(from, area, output, into)) {
        verdict.compare_exchange_strong(untried, Verdict::Works);
        return true;
    }

    const QString reason = QStringLiteral("%1 (%2)").arg(QLatin1String(card->refused)).arg(card->refusedWith);
    if (verdict.compare_exchange_strong(untried, Verdict::RuledOut))
        qCInfo(LogVideo).noquote() << "The graphics card does not scale pictures:" << reason << "; they are read back and scaled in software";
    else if (verdict == Verdict::Works)
        qCWarning(LogVideo).noquote() << "The graphics card did not scale a picture:" << reason;
    card->release();
    return false;
}

#else

struct CardScaler::Card
{
};

CardScaler::CardScaler() = default;

CardScaler::~CardScaler() = default;

bool CardScaler::usable()
{
    return false;
}

bool CardScaler::scale(const AVFrame *, const QRect &, const Output &, const QRect &, AVFrame *)
{
    return false;
}

#endif // Q_OS_WIN

bool CardScaler::scale(const AVFrame *from, const Output &output, AVFrame *into)
{
    const QRect area(static_cast<int>(from->crop_left), static_cast<int>(from->crop_top), from->width - static_cast<int>(from->crop_left + from->crop_right), from->height - static_cast<int>(from->crop_top + from->crop_bottom));
    return scale(from, area, output, QRect(QPoint(0, 0), output.size), into);
}

} // namespace Video
} // namespace Core
} // namespace Acheron
