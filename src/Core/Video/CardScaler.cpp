#include "Core/Video/CardScaler.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/CaptureCommon.hpp"

#include <QString>

#include <atomic>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#ifdef Q_OS_WIN
#include "Core/Video/Windows/Direct3D.hpp"
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

#ifdef Q_OS_WIN

using namespace Win;

namespace {

constexpr size_t MOST_PROCESSORS = 4;

RECT native(const QRect &rect)
{
    return { rect.left(), rect.top(), rect.left() + rect.width(), rect.top() + rect.height() };
}

} // namespace

struct CardScaler::Card
{
    struct Processor
    {
        QSize textureSize;
        DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
        ID3D11VideoProcessorEnumerator *enumerator = nullptr;
        ID3D11VideoProcessor *processor = nullptr;
    };

    ~Card() { release(); }

    [[nodiscard]] static bool takes(const AVFrame *from, const Output &output, const QRect &placed);
    [[nodiscard]] static bool wasTakenAway(const AVFrame *from);
    [[nodiscard]] QString refusal() const;

    bool scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &placed, AVFrame *into);
    const Processor *processorFor(ID3D11Texture2D *texture);
    bool choosePictures(const Processor &processor);
    Media::BufferPointer picturesOf(AVPixelFormat format, UINT bindFlags, const Processor &processor);
    void setUp(const Processor &processor);
    void dropProcessors();
    void release();

    Media::BufferPointer device;
    Output output;
    // an encoder relies on these staying the same while the card and output do
    Media::BufferPointer pictures;
    AVPixelFormat pictureFormat = AV_PIX_FMT_NONE;
    std::vector<Processor> processors;
    const char *refused = "";
    HRESULT refusedWith = S_OK;
};

bool CardScaler::Card::takes(const AVFrame *from, const Output &output, const QRect &)
{
    return from->format == AV_PIX_FMT_D3D11 && from->hw_frames_ctx && !output.size.isEmpty();
}

// a card Windows just took away (driver or mode change) has said nothing about what it can do
bool CardScaler::Card::wasTakenAway(const AVFrame *from)
{
    return FAILED(deviceOf(from)->device->GetDeviceRemovedReason());
}

QString CardScaler::Card::refusal() const
{
    return QStringLiteral("%1 (%2)").arg(QLatin1String(refused), QString::number(static_cast<quint32>(refusedWith), 16));
}

void CardScaler::Card::dropProcessors()
{
    for (Processor &each : processors) {
        drop(each.processor);
        drop(each.enumerator);
    }
    processors.clear();
}

void CardScaler::Card::release()
{
    dropProcessors();
    pictures.reset();
    device.reset();
    pictureFormat = AV_PIX_FMT_NONE;
}

bool CardScaler::Card::scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &placed, AVFrame *into)
{
    const auto *source = reinterpret_cast<const AVHWFramesContext *>(from->hw_frames_ctx->data);
    if (!device || device->data != source->device_ref->data || !(wanted == output)) {
        release();
        device.reset(av_buffer_ref(source->device_ref));
        output = wanted;
    }
    refusedWith = S_OK;
    refused = "it has no video processor";
    AVD3D11VADeviceContext *d3d = device ? direct3d(device.get()) : nullptr;
    if (!d3d || !d3d->video_device || !d3d->video_context)
        return false;

    ID3D11Texture2D *texture = textureOf(from);
    const Processor *processor = processorFor(texture);
    if (!processor)
        return false;

    av_frame_unref(into);
    refused = "it made no texture for a scaled picture";
    if (av_hwframe_get_buffer(pictures.get(), into, 0) < 0)
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC sourceView = {};
    sourceView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    sourceView.Texture2D.ArraySlice = sliceOf(from);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC targetView = {};
    targetView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorInputView *input = nullptr;
    ID3D11VideoProcessorOutputView *target = nullptr;

    refused = "its video processor does not read the picture";
    refusedWith = d3d->video_device->CreateVideoProcessorInputView(texture, processor->enumerator, &sourceView, &input);
    if (SUCCEEDED(refusedWith)) {
        refused = "its video processor does not draw into the scaled picture";
        refusedWith = d3d->video_device->CreateVideoProcessorOutputView(textureOf(into), processor->enumerator, &targetView, &target);
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

    markColours(into, pictureFormat == AV_PIX_FMT_NV12 ? output.matrix : ColourMatrix::Bt709);
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
    if (processors.size() >= MOST_PROCESSORS)
        dropProcessors();

    Processor made;
    made.textureSize = textureSize;
    made.textureFormat = description.Format;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = processorContent(textureSize, output.size);

    ID3D11VideoDevice *video = direct3d(device.get())->video_device;
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

        // not every driver makes a texture that is both a render target and encoder input
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

Media::BufferPointer CardScaler::Card::picturesOf(AVPixelFormat format, UINT bindFlags, const Processor &processor)
{
    Media::BufferPointer made(av_hwframe_ctx_alloc(device.get()));
    if (!made)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(made->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = format;
    frames->width = output.size.width();
    frames->height = output.size.height();
    // a pool size makes one texture array, which a Radeon wont make of NV12 render targets
    frames->initial_pool_size = 0;
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = bindFlags;

    const Media::FramePointer first(av_frame_alloc());
    if (!first || av_hwframe_ctx_init(made.get()) < 0 || av_hwframe_get_buffer(made.get(), first.get(), 0) < 0)
        return nullptr;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC description = {};
    description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView *view = nullptr;
    const bool drawable = SUCCEEDED(direct3d(device.get())->video_device->CreateVideoProcessorOutputView(textureOf(first.get()), processor.enumerator, &description, &view));
    drop(view);
    if (!drawable)
        return nullptr;
    return made;
}

void CardScaler::Card::setUp(const Processor &processor)
{
    AVD3D11VADeviceContext *d3d = direct3d(device.get());
    ID3D11VideoContext *context = d3d->video_context;
    const RECT whole = native(QRect(QPoint(0, 0), output.size));
    D3D11_VIDEO_COLOR black = {};
    black.RGBA.A = 1;

    const DeviceLock lock(d3d);
    context->VideoProcessorSetStreamFrameFormat(processor.processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // drivers otherwise sharpen and denoise what they take for film
    context->VideoProcessorSetStreamAutoProcessingMode(processor.processor, 0, FALSE);
    context->VideoProcessorSetOutputTargetRect(processor.processor, TRUE, &whole);
    context->VideoProcessorSetOutputBackgroundColor(processor.processor, FALSE, &black);
    const DXGI_COLOR_SPACE_TYPE yuv = output.matrix == ColourMatrix::Bt601 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    setColourSpaces(context, processor.processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, pictureFormat == AV_PIX_FMT_NV12 ? yuv : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

#elif defined(ACHERON_HAVE_AVFILTER)

struct CardScaler::Card
{
    ~Card() { release(); }

    [[nodiscard]] static bool takes(const AVFrame *from, const Output &output, const QRect &placed);
    [[nodiscard]] static bool wasTakenAway(const AVFrame *) { return false; }
    [[nodiscard]] QString refusal() const;

    bool scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &placed, AVFrame *into);
    bool build(const AVFrame *from);
    void release();

    Media::BufferPointer device;
    Output output;
    QSize sourceSize;
    AVPixelFormat sourceFormat = AV_PIX_FMT_NONE;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr;
    AVFilterContext *sink = nullptr;
    const Media::FramePointer cut{ av_frame_alloc() };
    const char *refused = "";
    int refusedWith = 0;
};

bool CardScaler::Card::takes(const AVFrame *from, const Output &output, const QRect &placed)
{
    return from->format == AV_PIX_FMT_VAAPI && from->hw_frames_ctx && !output.size.isEmpty() && placed == QRect(QPoint(0, 0), output.size);
}

QString CardScaler::Card::refusal() const
{
    return QStringLiteral("%1 (%2)").arg(QLatin1String(refused)).arg(refusedWith);
}

void CardScaler::Card::release()
{
    avfilter_graph_free(&graph);
    source = nullptr;
    sink = nullptr;
    device.reset();
}

bool CardScaler::Card::scale(const AVFrame *from, const QRect &area, const Output &wanted, const QRect &, AVFrame *into)
{
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(from->hw_frames_ctx->data);
    const QSize size(from->width, from->height);
    if (!graph || device->data != frames->device_ref->data || !(wanted == output) || size != sourceSize || frames->sw_format != sourceFormat) {
        release();
        device.reset(av_buffer_ref(frames->device_ref));
        output = wanted;
        sourceSize = size;
        sourceFormat = frames->sw_format;
        if (!build(from))
            return false;
    }

    const QRect taken = area & QRect(QPoint(0, 0), size);
    av_frame_unref(cut.get());
    refused = "it would not take the picture";
    refusedWith = av_frame_ref(cut.get(), from);
    if (refusedWith < 0)
        return false;
    cropTo(cut.get(), taken);
    cut->color_range = AVCOL_RANGE_JPEG;

    refusedWith = av_buffersrc_add_frame_flags(source, cut.get(), AV_BUFFERSRC_FLAG_PUSH | AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT);
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

    const QByteArray settings = QStringLiteral("w=%1:h=%2:format=nv12:out_color_matrix=%3:out_range=limited").arg(output.size.width()).arg(output.size.height()).arg(QLatin1String(output.matrix == ColourMatrix::Bt601 ? "smpte170m" : "bt709")).toLatin1();
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

#endif

#if defined(Q_OS_WIN) || defined(ACHERON_HAVE_AVFILTER)

namespace {

enum class Verdict {
    Untried,
    Works,
    RuledOut,
};
std::atomic<Verdict> verdict{ Verdict::Untried };

} // namespace

CardScaler::CardScaler() : card(std::make_unique<Card>()) {}

CardScaler::~CardScaler() = default;

bool CardScaler::usable()
{
    return verdict != Verdict::RuledOut;
}

bool CardScaler::scale(const AVFrame *from, const QRect &area, const Output &output, const QRect &placed, AVFrame *into)
{
    if (!Card::takes(from, output, placed))
        return false;

    Verdict untried = Verdict::Untried;
    if (card->scale(from, area, output, placed, into)) {
        verdict.compare_exchange_strong(untried, Verdict::Works);
        return true;
    }

    const bool cardTakenAway = Card::wasTakenAway(from);
    if (!cardTakenAway && verdict.compare_exchange_strong(untried, Verdict::RuledOut))
        qCInfo(LogVideo).noquote() << "The graphics card does not scale pictures:" << card->refusal() << "; they are read back and scaled in software";
    else if (cardTakenAway || verdict == Verdict::Works)
        qCWarning(LogVideo).noquote() << "The graphics card did not scale a picture:" << card->refusal();
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

#endif

bool CardScaler::scale(const AVFrame *from, const Output &output, AVFrame *into)
{
    return scale(from, visibleArea(from), output, QRect(QPoint(0, 0), output.size), into);
}

} // namespace Video
} // namespace Core
} // namespace Acheron
