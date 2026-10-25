// what the screen shows is only counted and measured here: never printed or written anywhere

#include "Core/Media/CodecSupport.hpp"
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/ScreenPipeline.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "ColourChecks.hpp"
#include "EncodedFrameChecks.hpp"
#include "ProcessUsage.hpp"
#include "HardwareTests.hpp"

#include <QElapsedTimer>
#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#ifdef Q_OS_WIN
#include <windows.h>

#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#else
#include <ctime>
#endif

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
#include "PipeWireTestSource.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>

#include <mutex>
#endif

#if !defined(Q_OS_WIN) && defined(ACHERON_HAVE_XCB) && defined(ACHERON_HAVE_AVDEVICE)
#define ACHERON_TESTS_X11
#include "Core/Video/LinuxCapture.hpp"
#include "X11TestWindow.hpp"
#endif

using namespace Acheron;
using namespace Acheron::Discord::Voice;
using Core::Media::VideoEncoder;
using namespace EncodedFrameChecks;
using ProcessUsage::processCpuMs;

namespace {

using ColourChecks::distance;
using ColourChecks::inYuv;
using ColourChecks::Yuv;

// the height columns hold 0 for the source's own height, as the quality picker stores it
std::optional<int> limitOf(int maxHeight)
{
    return maxHeight == 0 ? std::nullopt : std::optional<int>(maxHeight);
}

constexpr int WIDTH = 640;
constexpr int HEIGHT = 360;
constexpr int FRAMERATE = 30;
constexpr int TICKS_PER_FRAME = 90000 / FRAMERATE;
constexpr int BITRATE = 1'500'000;

constexpr std::array CODECS = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

constexpr int PATCH_RED = 40;
constexpr int PATCH_GREEN = 200;
constexpr int PATCH_BLUE = 60;

struct EncoderCase
{
    VideoCodec codec = VideoCodec::Unknown;
    QString encoderName;
    int detail = 0;
};

bool isOfTheGraphicsCard(const QString &encoderName)
{
    for (const char *ending : { "_amf", "_nvenc", "_qsv", "_vaapi", " hardware" }) {
        if (encoderName.endsWith(QLatin1String(ending)))
            return true;
    }
    return false;
}

QList<QString> encodersToTest(VideoCodec codec)
{
    QList<QString> names = Core::Media::videoEncoderNames(codec);
    if (!hardwareTestsAreAskedFor())
        names.erase(std::remove_if(names.begin(), names.end(), isOfTheGraphicsCard), names.end());
    return names;
}

Yuv patchInYuv(bool bt709)
{
    return inYuv(PATCH_RED, PATCH_GREEN, PATCH_BLUE, bt709);
}

class CountingSink : public Core::Media::VideoFrameSink
{
public:
    static constexpr int TOP_ROW = 4;

    void present(const AVFrame *frame) override
    {
        if (inspect)
            inspect(frame, frames);
        width = frame->width;
        height = frame->height;
        format = frame->format;
        colorspace = frame->colorspace;
        range = frame->color_range;
        shapeWidth = frame->sample_aspect_ratio.num;
        shapeHeight = frame->sample_aspect_ratio.den;
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            middleY = frame->data[0][y * frame->linesize[0] + x];
            middleU = frame->data[1][(y / 2) * frame->linesize[1] + x / 2];
            middleV = frame->data[2][(y / 2) * frame->linesize[2] + x / 2];
            topY = frame->data[0][TOP_ROW * frame->linesize[0] + x];
            topU = frame->data[1][(TOP_ROW / 2) * frame->linesize[1] + x / 2];
            topV = frame->data[2][(TOP_ROW / 2) * frame->linesize[2] + x / 2];
        }
        frames++;
    }

    std::function<void(const AVFrame *, int)> inspect;
    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
    std::atomic<int> format{ AV_PIX_FMT_NONE };
    std::atomic<int> colorspace{ AVCOL_SPC_UNSPECIFIED };
    std::atomic<int> range{ AVCOL_RANGE_UNSPECIFIED };
    std::atomic<int> shapeWidth{ 0 };
    std::atomic<int> shapeHeight{ 1 };
    std::atomic<int> middleY{ 0 };
    std::atomic<int> middleU{ 0 };
    std::atomic<int> middleV{ 0 };
    std::atomic<int> topY{ 0 };
    std::atomic<int> topU{ 0 };
    std::atomic<int> topV{ 0 };
};

void paintYuv(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < width / 4 && y < height / 6) {
                noise = noise * 1664525u + 1013904223u;
                value = static_cast<uint8_t>(96 + (noise >> 26));
            }
            row[x] = value;
        }
    }

    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            const auto u = static_cast<uint8_t>(128 + y / 4 + index * 2);
            const auto v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

void paintBgra(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            const bool inPatch = std::abs(x - width / 2) < width / 8 && std::abs(y - height / 2) < height / 8;
            row[4 * x] = inPatch ? PATCH_BLUE : static_cast<uint8_t>(x / 2 + index * 3);
            row[4 * x + 1] = inPatch ? PATCH_GREEN : static_cast<uint8_t>(y / 2 + index * 2);
            row[4 * x + 2] = inPatch ? PATCH_RED : static_cast<uint8_t>((x + y) / 4);
            row[4 * x + 3] = 255;
        }
    }
}

void paint(AVFrame *picture, int index)
{
    av_frame_make_writable(picture);
    if (picture->format == AV_PIX_FMT_BGRA)
        paintBgra(picture, index);
    else
        paintYuv(picture, index);
}

AVFrame *allocatePicture(int format, int width, int height)
{
    AVFrame *picture = av_frame_alloc();
    picture->format = format;
    picture->width = width;
    picture->height = height;
    av_frame_get_buffer(picture, 0);
    return picture;
}

VideoEncoder::Settings settingsFor(const EncoderCase &encoderCase, int bitrate)
{
    VideoEncoder::Settings settings;
    settings.codec = encoderCase.codec;
    settings.framerate = FRAMERATE;
    settings.bitrateBps = bitrate;
    settings.encoderName = encoderCase.encoderName;
    return settings;
}

class EncodeRun
{
public:
    EncodeRun(const EncoderCase &encoderCase, int width, int height, int bitrate)
        : encoder(settingsFor(encoderCase, bitrate)), picture(allocatePicture(encoder.softwareFormat(), width, height))
    {
    }

    ~EncodeRun() { av_frame_free(&picture); }

    void paintNext() { paint(picture, sent); }

    bool encodeNext(bool painting = true)
    {
        if (painting)
            paintNext();
        const size_t before = frames.size();
        const uint32_t timestamp = static_cast<uint32_t>(sent) * TICKS_PER_FRAME;

        if (encoder.encode(picture, timestamp, frames) != VideoEncoder::Outcome::Encoded)
            return false;

        producedItsOwnFrame.push_back(frames.size() == before + 1 && frames.back().rtpTimestamp == timestamp);
        sent++;
        return true;
    }

    [[nodiscard]] double bitsPerSecond(size_t from, size_t to, int pictures) const
    {
        size_t bytes = 0;
        for (size_t i = from; i < to; i++)
            bytes += frames[i].data.size();
        return static_cast<double>(bytes) * 8.0 * FRAMERATE / pictures;
    }

    VideoEncoder encoder;
    std::vector<EncodedVideoFrame> frames;
    std::vector<bool> producedItsOwnFrame;
    int sent = 0;

private:
    AVFrame *picture;
};

AVHWDeviceType cardDevice(const QString &encoderName = {})
{
#ifdef Q_OS_WIN
    Q_UNUSED(encoderName);
    return AV_HWDEVICE_TYPE_D3D11VA;
#else
    return encoderName.endsWith(QLatin1String("_nvenc")) ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;
#endif
}

AVPixelFormat cardFormat(AVHWDeviceType device)
{
    if (device == AV_HWDEVICE_TYPE_D3D11VA)
        return AV_PIX_FMT_D3D11;
    return device == AV_HWDEVICE_TYPE_CUDA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
}

QString onDevice(AVHWDeviceType device)
{
    return QStringLiteral("(%1)").arg(QLatin1String(av_hwdevice_get_type_name(device)));
}

AVBufferRef *framesOnCard(AVBufferRef *device, AVPixelFormat format, const QSize &size, int poolSize = 0)
{
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
    context->format = cardFormat(reinterpret_cast<AVHWDeviceContext *>(device->data)->type);
    context->sw_format = format;
    context->width = size.width();
    context->height = size.height();
    context->initial_pool_size = poolSize;
#ifdef Q_OS_WIN
    static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
#endif
    if (av_hwframe_ctx_init(frames) < 0)
        av_buffer_unref(&frames);
    return frames;
}

class CardPictures
{
public:
    CardPictures(AVPixelFormat format, int width, int height, int colorspace, AVHWDeviceType type = cardDevice())
        : device(onDevice(type)), colorspace(colorspace), inMemory(allocatePicture(format, width, height)), onCard(av_frame_alloc())
    {
        AVBufferRef *made = nullptr;
        if (!Core::Media::hardwareDeviceCanBeAskedFor(type) || av_hwdevice_ctx_create(&made, type, nullptr, nullptr, 0) < 0)
            return;
        frames = framesOnCard(made, format, QSize(width, height));
        av_buffer_unref(&made);
    }

    ~CardPictures()
    {
        av_frame_free(&inMemory);
        av_frame_free(&onCard);
        av_buffer_unref(&frames);
    }

    [[nodiscard]] bool usable() const { return frames != nullptr; }

    AVFrame *next(int index)
    {
        paint(inMemory, index);
        av_frame_unref(onCard);
        if (av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
            return nullptr;
        onCard->colorspace = static_cast<AVColorSpace>(colorspace);
        onCard->color_range = AVCOL_RANGE_MPEG;
        return onCard;
    }

    const QString device;

private:
    int colorspace;
    AVBufferRef *frames = nullptr;
    AVFrame *inMemory;
    AVFrame *onCard;
};

struct Rgb
{
    int red;
    int green;
    int blue;
};

constexpr std::array QUARTER_COLOURS = { Rgb{ 200, 40, 40 }, Rgb{ 40, 200, 60 }, Rgb{ 40, 60, 220 }, Rgb{ 128, 128, 128 } };

AVFrame *quartersOnCard(AVBufferRef *device, const QSize &size)
{
    AVFrame *inMemory = allocatePicture(AV_PIX_FMT_BGRA, size.width(), size.height());
    for (int y = 0; y < size.height(); y++) {
        uint8_t *row = inMemory->data[0] + y * inMemory->linesize[0];
        const bool lower = y >= size.height() / 2;
        for (int x = 0; x < size.width(); x++) {
            const Rgb colour = QUARTER_COLOURS[(lower ? 2 : 0) + (x >= size.width() / 2 ? 1 : 0)];
            row[4 * x] = static_cast<uint8_t>(colour.blue);
            row[4 * x + 1] = static_cast<uint8_t>(colour.green);
            row[4 * x + 2] = static_cast<uint8_t>(colour.red);
            row[4 * x + 3] = lower ? 0 : 255;
        }
    }

    AVBufferRef *frames = framesOnCard(device, AV_PIX_FMT_BGRA, size);
    AVFrame *onCard = av_frame_alloc();
    if (!frames || av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
        av_frame_free(&onCard);
    if (onCard)
        onCard->color_range = AVCOL_RANGE_MPEG;
    av_buffer_unref(&frames);
    av_frame_free(&inMemory);
    return onCard;
}

AVPixelFormat formatOnCard(const AVFrame *card)
{
    return card->hw_frames_ctx ? reinterpret_cast<const AVHWFramesContext *>(card->hw_frames_ctx->data)->sw_format : AV_PIX_FMT_NONE;
}

Yuv colourAt(const AVFrame *nv12, int x, int y)
{
    const uint8_t *chroma = nv12->data[1] + (y / 2) * nv12->linesize[1] + (x / 2) * 2;
    return { static_cast<double>(nv12->data[0][y * nv12->linesize[0] + x]), static_cast<double>(chroma[0]), static_cast<double>(chroma[1]) };
}

Yuv inYuv(const Rgb &colour, bool bt709)
{
    return inYuv(colour.red, colour.green, colour.blue, bt709);
}

#ifdef Q_OS_WIN
Yuv inYuv(COLORREF colour, bool bt709)
{
    return inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), bt709);
}
#endif

QString unitTypes(VideoCodec codec, const EncodedVideoFrame &frame)
{
    QStringList types;
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(frame.data))
            types.append(QString::number(obu.type));
    } else if (codec != VideoCodec::VP8) {
        for (int type : nalTypes(codec, frame.data))
            types.append(QString::number(type));
    }
    return types.join(QLatin1Char(' '));
}

int profileOf(VideoCodec codec, const std::vector<uint8_t> &data)
{
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(data)) {
            if (obu.type == OBU_SEQUENCE_HEADER && obu.size > 0)
                return data[obu.payload] >> 5;
        }
        return -1;
    }
    for (size_t i = 0; i + 6 < data.size(); i++) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (codec == VideoCodec::H264 && (data[i + 3] & 0x1F) == H264_SPS)
            return data[i + 4];
        if (codec == VideoCodec::H265 && ((data[i + 3] >> 1) & 0x3F) == H265_SPS)
            return data[i + 6] & 0x1F;
    }
    return -1;
}

struct Decoded
{
    int pictures = 0;
    int width = 0;
    int height = 0;
    quint64 errors = 0;
    quint64 dropped = 0;
    int format = AV_PIX_FMT_NONE;
    int colorspace = AVCOL_SPC_UNSPECIFIED;
    int range = AVCOL_RANGE_UNSPECIFIED;
    AVRational shape = { 0, 1 };
    Yuv middle;
    Yuv top;
};

bool isThePictureSent(VideoCodec codec, const Decoded &decoded, const QSize &sent)
{
    if (QSize(decoded.width, decoded.height) == sent)
        return true;
    return codec == VideoCodec::AV1 && decoded.width >= sent.width() && decoded.height >= sent.height() && decoded.width - sent.width() < 64 && decoded.height - sent.height() < 16;
}

template <typename Frames>
Decoded decodeAll(VideoCodec codec, const Frames &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    auto sink = std::make_shared<CountingSink>();
    sink->inspect = std::move(inspect);
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);

    int pushed = 0;
    QElapsedTimer timer;
    timer.start();
    for (const auto &frame : frames) {
        decoder.push(codec, EncodedVideoFrame(*frame));
        pushed++;
        // the decoder discards its queue when it falls far behind
        while (pushed - sink->frames > 20 && timer.elapsed() < 10000)
            QThread::msleep(1);
    }
    while (sink->frames < pushed && timer.elapsed() < 10000)
        QThread::msleep(2);
    QThread::msleep(50);

    Decoded decoded;
    decoded.pictures = sink->frames;
    decoded.width = sink->width;
    decoded.height = sink->height;
    decoded.errors = decoder.stats().errors;
    decoded.dropped = decoder.stats().dropped;
    decoded.format = sink->format;
    decoded.colorspace = sink->colorspace;
    decoded.range = sink->range;
    decoded.shape = { sink->shapeWidth, sink->shapeHeight };
    decoded.middle = { static_cast<double>(sink->middleY), static_cast<double>(sink->middleU), static_cast<double>(sink->middleV) };
    decoded.top = { static_cast<double>(sink->topY), static_cast<double>(sink->topU), static_cast<double>(sink->topV) };
    return decoded;
}

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedVideoFrame> &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    std::vector<const EncodedVideoFrame *> pointers;
    for (const EncodedVideoFrame &frame : frames)
        pointers.push_back(&frame);
    return decodeAll(codec, pointers, std::move(inspect));
}

struct PipelineRecord
{
    explicit PipelineRecord(Core::Video::ScreenPipeline &pipeline)
    {
        using Core::Video::ScreenPipeline;
        clock.start();
        QObject::connect(&pipeline, &ScreenPipeline::started, &here, [this](const QSize &size, int) { starts.append(size); });
        QObject::connect(&pipeline, &ScreenPipeline::frameEncoded, &here, [this](const Core::Video::EncodedFramePointer &frame) {
            frames.push_back(frame);
            arrivedAt.push_back(clock.elapsed());
        });
        QObject::connect(&pipeline, &ScreenPipeline::statsUpdated, &here, [this](const Core::Video::PipelineStats &update) { stats.append(update); });
        QObject::connect(&pipeline, &ScreenPipeline::captureStalled, &here, [this](bool stalled) { stalls.append(stalled); });
        QObject::connect(&pipeline, &ScreenPipeline::sourceClosed, &here, [this] { closes++; });
        QObject::connect(&pipeline, &ScreenPipeline::failed, &here, [this](const QString &reason) { failures.append(reason); });
    }

    [[nodiscard]] std::vector<Core::Video::EncodedFramePointer> framesBefore(size_t end) const
    {
        return std::vector<Core::Video::EncodedFramePointer>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(end));
    }

    [[nodiscard]] size_t keyFrameFrom(size_t from) const
    {
        size_t index = from;
        while (index < frames.size() && !frames[index]->keyframe)
            index++;
        return index;
    }

    QObject here;
    QElapsedTimer clock;
    std::vector<Core::Video::EncodedFramePointer> frames;
    std::vector<qint64> arrivedAt;
    QList<QSize> starts;
    QList<Core::Video::PipelineStats> stats;
    QList<bool> stalls;
    QStringList failures;
    int closes = 0;
};

struct PathCounts
{
    qint64 cutOut = -1;
    qint64 drawn = -1;
};

PathCounts pathCounts(const Core::Video::PipelineStats &stats)
{
    static const QRegularExpression pattern(QStringLiteral("ddagrab (\\d+) \\+ PrintWindow (\\d+) frames"));
    const QRegularExpressionMatch match = pattern.match(stats.encoderName);
    return match.hasMatch() ? PathCounts{ match.captured(1).toLongLong(), match.captured(2).toLongLong() } : PathCounts{};
}

QList<VideoCodec> codecsSharedAt(const QSize &size)
{
    return Core::Media::shareCodecs(Core::Media::encodableVideoCodecs(), size);
}

bool reachesTheEncoderOnTheCard(VideoCodec codec, bool scaled, bool byGraphicsCapture)
{
    const VideoEncoder chosen(settingsFor({ codec, {} }, BITRATE));
    const bool withoutTheProcessor = chosen.takesRgbCardFrames() && (!scaled || byGraphicsCapture);
    return chosen.takesCardFrames() && (withoutTheProcessor || Core::Video::CardScaler::usable());
}

bool aScreenIsOnAGraphicsCard()
{
#ifdef Q_OS_WIN
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;

    bool found = false;
    IDXGIAdapter *adapter = nullptr;
    for (UINT index = 0; !found && SUCCEEDED(factory->EnumAdapters(index, &adapter)); index++) {
        IDXGIOutput *output = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            output->Release();
            AVBufferRef *device = nullptr;
            found = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(index).constData(), nullptr, 0) >= 0;
            av_buffer_unref(&device);
        }
        adapter->Release();
    }
    factory->Release();
    return found;
#else
    return true;
#endif
}

#ifdef Q_OS_WIN
constexpr COLORREF SHARED_COLOUR = RGB(PATCH_RED, PATCH_GREEN, PATCH_BLUE);
constexpr COLORREF COVERING_COLOUR = RGB(220, 40, 200);
constexpr int BLACK_AT_MOST = 40;

bool looksLike(const Yuv &seen, COLORREF colour)
{
    constexpr double CLOSE = 16;
    return distance(seen, inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), true)) < CLOSE || distance(seen, inYuv(GetRValue(colour), GetGValue(colour), GetBValue(colour), false)) < CLOSE;
}

QString captureNameOf(Core::Video::CaptureMethod method)
{
    switch (method) {
    case Core::Video::CaptureMethod::GraphicsCapture:
        return QStringLiteral("gfxcapture");
    case Core::Video::CaptureMethod::ScreenCopy:
        return QStringLiteral("BitBlt");
    default:
        return QStringLiteral("ddagrab");
    }
}

double offItsOwnCoefficients(const Decoded &decoded, const Yuv &seen, COLORREF colour)
{
    const bool named = decoded.colorspace == AVCOL_SPC_BT709 || decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG;
    return distance(seen, inYuv(colour, named ? decoded.colorspace == AVCOL_SPC_BT709 : decoded.height >= 720));
}
constexpr double COLOUR_SURVIVES = 8;

void measureInDesktopPixels(bool desktopPixels)
{
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    static const auto setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    if (setAwareness)
        setAwareness(desktopPixels ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : DPI_AWARENESS_CONTEXT_UNAWARE);
}

struct QuietScreen
{
    QRect whole;
    QRect workArea;

    [[nodiscard]] QPoint cornerFor(const QSize &room) const
    {
        constexpr int MARGIN = 24;
        return QPoint(std::max(workArea.left(), workArea.left() + workArea.width() - room.width() - MARGIN), std::max(workArea.top(), workArea.top() + workArea.height() - room.height() - MARGIN));
    }
};

BOOL CALLBACK takeQuietScreen(HMONITOR monitor, HDC, LPRECT, LPARAM found)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    // not a turned screen: duplication hands it over on its side
    if (!GetMonitorInfoW(monitor, &info) || (info.dwFlags & MONITORINFOF_PRIMARY) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayOrientation != DMDO_DEFAULT)
        return TRUE;

    const auto rect = [](const RECT &native) { return QRect(native.left, native.top, native.right - native.left, native.bottom - native.top); };
    *reinterpret_cast<std::optional<QuietScreen> *>(found) = QuietScreen{ rect(info.rcMonitor), rect(info.rcWork) };
    return FALSE;
}

std::optional<QuietScreen> quietScreen()
{
    std::optional<QuietScreen> found;
    EnumDisplayMonitors(nullptr, nullptr, takeQuietScreen, reinterpret_cast<LPARAM>(&found));
    return found;
}

#define SKIP_WITHOUT_A_QUIET_SCREEN(screen)                  \
    const std::optional<QuietScreen> screen = quietScreen(); \
    if (!screen)                                             \
    QSKIP("the windows this opens go on a second screen, out of the way, and this machine has one")

class TestWindow
{
public:
    struct Look
    {
        QSize inside;
        QPoint at;
        COLORREF colour = SHARED_COLOUR;
        bool framed = true;
        bool scaledByWindows = false;
    };

    explicit TestWindow(const Look &look) : look(look), thread([this] { run(); })
    {
        while (!window && !failedToOpen)
            QThread::msleep(5);
        if (!window)
            return;
        if (look.framed)
            resizeInside(look.inside);
        moveTo(look.at);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    ~TestWindow()
    {
        if (isOpen())
            PostMessageW(window, WM_CLOSE, 0, 0);
        thread.join();
    }

    [[nodiscard]] HWND handle() const { return window; }
    [[nodiscard]] bool isOpen() const { return window && IsWindow(window); }
    [[nodiscard]] QString sourceId() const { return QStringLiteral("window:%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(window.load()))); }

    [[nodiscard]] QRect insideOnScreen() const
    {
        RECT inside = {};
        POINT corner = {};
        GetClientRect(window, &inside);
        ClientToScreen(window, &corner);
        return QRect(corner.x, corner.y, inside.right, inside.bottom);
    }

    void setAnimated(bool on) { animated = on; }

    void resizeInside(const QSize &size)
    {
        RECT whole = {};
        GetWindowRect(window, &whole);
        const QSize grown = QSize(whole.right - whole.left, whole.bottom - whole.top) + size - insideOnScreen().size();
        SetWindowPos(window, nullptr, 0, 0, grown.width(), grown.height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void moveTo(const QPoint &at) { SetWindowPos(window, nullptr, at.x(), at.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    void show() { ShowWindowAsync(window, SW_SHOWNOACTIVATE); }
    void hide() { ShowWindowAsync(window, SW_HIDE); }
    void close() { PostMessageW(window, WM_CLOSE, 0, 0); }
    bool keepOutOfCaptures(bool keep) { return SetWindowDisplayAffinity(window, keep ? WDA_MONITOR : WDA_NONE) != FALSE; }

    bool reachPastAnOuterEdge(const QRect &screen, int pixels)
    {
        struct Edge
        {
            QPoint insideAt;
            QRect beyond;
        };
        constexpr int FRAME_ALLOWANCE = 64;
        const QRect inside = insideOnScreen();
        RECT whole = {};
        GetWindowRect(window, &whole);
        const QSize outer(whole.right - whole.left, whole.bottom - whole.top);
        const int reach = pixels + FRAME_ALLOWANCE;
        const int right = screen.left() + screen.width();
        const int bottom = screen.top() + screen.height();
        const std::array<Edge, 4> edges = {
            Edge{ QPoint(screen.left() - pixels, inside.top()), QRect(screen.left() - reach, whole.top, reach, outer.height()) },
            Edge{ QPoint(right + pixels - inside.width(), inside.top()), QRect(right, whole.top, reach, outer.height()) },
            Edge{ QPoint(inside.left(), screen.top() - pixels), QRect(whole.left, screen.top() - reach, outer.width(), reach) },
            Edge{ QPoint(inside.left(), bottom + pixels - inside.height()), QRect(whole.left, bottom, outer.width(), reach) },
        };
        for (const Edge &edge : edges) {
            const RECT beyond = { edge.beyond.left(), edge.beyond.top(), edge.beyond.left() + edge.beyond.width(), edge.beyond.top() + edge.beyond.height() };
            if (MonitorFromRect(&beyond, MONITOR_DEFAULTTONULL))
                continue;
            moveTo(edge.insideAt - (inside.topLeft() - QPoint(whole.left, whole.top)));
            return true;
        }
        return false;
    }

    void setCloaked(bool cloaked)
    {
        constexpr DWORD CLOAK_ATTRIBUTE = 13;
        using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = reinterpret_cast<SetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute")));
        const BOOL value = cloaked;
        if (setAttribute)
            setAttribute(window, CLOAK_ATTRIBUTE, &value, sizeof(value));
    }

private:
    static void fill(HDC context, const RECT &area, COLORREF colour)
    {
        const HBRUSH brush = CreateSolidBrush(colour);
        FillRect(context, &area, brush);
        DeleteObject(brush);
    }

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        constexpr RECT CORNER = { 0, 0, 24, 24 };
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW *>(lParam)->lpCreateParams));
        auto *self = reinterpret_cast<TestWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message) {
        case WM_TIMER:
            if (self && self->animated) {
                self->tick++;
                InvalidateRect(window, &CORNER, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT painting;
            const HDC context = BeginPaint(window, &painting);
            RECT inside = {};
            GetClientRect(window, &inside);
            if (self) {
                fill(context, inside, self->look.colour);
                fill(context, CORNER, RGB(self->tick * 37 & 255, self->tick * 91 & 255, self->tick * 13 & 255));
            }
            EndPaint(window, &painting);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run()
    {
        static const wchar_t *const CLASS_NAME = L"AcheronCaptureTestWindow";
        measureInDesktopPixels(!look.scaledByWindows);

        WNDCLASSW description = {};
        description.style = CS_HREDRAW | CS_VREDRAW;
        description.lpfnWndProc = dispatch;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = CLASS_NAME;
        RegisterClassW(&description);

        const HWND owner = CreateWindowExW(0, CLASS_NAME, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, description.hInstance, nullptr);
        const HWND made = CreateWindowExW(WS_EX_TOPMOST, CLASS_NAME, L"Acheron capture test", look.framed ? WS_OVERLAPPEDWINDOW : WS_POPUP, look.at.x(), look.at.y(), look.inside.width(), look.inside.height(), owner, nullptr, description.hInstance, this);
        if (!made) {
            failedToOpen = true;
            return;
        }
        SetTimer(made, 1, 16, nullptr);
        window = made;

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    const Look look;
    std::atomic<HWND> window{ nullptr };
    std::atomic<bool> failedToOpen{ false };
    std::atomic<bool> animated{ true };
    int tick = 0;
    std::thread thread;
};
#endif

#if (defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)) || defined(ACHERON_TESTS_X11)
constexpr Rgb SHARED_COLOUR = { PATCH_RED, PATCH_GREEN, PATCH_BLUE };
constexpr double COLOUR_SURVIVES = 8;
constexpr QSize TINY_WINDOW(100, 80);
constexpr QSize SMALLEST_PICTURE(160, 120);

bool looksLike(const Yuv &seen, const Rgb &colour)
{
    constexpr double CLOSE = 16;
    return distance(seen, inYuv(colour, true)) < CLOSE || distance(seen, inYuv(colour, false)) < CLOSE;
}

double offItsOwnCoefficients(VideoCodec codec, const Decoded &decoded, const Yuv &seen, const Rgb &colour)
{
    const bool named = decoded.colorspace == AVCOL_SPC_BT709 || decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG;
    // VP8 cant name its colours and is BT.601 at every size
    const bool bt709 = codec != VideoCodec::VP8 && (named ? decoded.colorspace == AVCOL_SPC_BT709 : decoded.height >= 720);
    return distance(seen, inYuv(colour, bt709));
}
#endif

#ifdef ACHERON_TESTS_X11
constexpr Rgb COVERING_COLOUR = { 220, 40, 200 };
constexpr int BLACK_AT_MOST = 40;
const char NO_X11[] = "no X11 display shows this desktop: there is none, or it is Xwayland";
const char TINY_FROM_X11[] = "grabbed from X11, a window too small to be sent as it is";

using TestWindow = X11TestWindow;

struct QuietScreen
{
    QRect whole;
    QRect workArea;

    [[nodiscard]] QPoint cornerFor(const QSize &room) const
    {
        constexpr int MARGIN = 24;
        return QPoint(std::max(workArea.left(), workArea.left() + workArea.width() - room.width() - MARGIN), std::max(workArea.top(), workArea.top() + workArea.height() - room.height() - MARGIN));
    }
};

std::optional<QuietScreen> quietScreen()
{
    const QList<Core::Video::CaptureSource> screens = Core::Video::X11Capture::screens();
    const TestWindow::Display display;
    if (screens.isEmpty() || !display.isOpen() || (screens.size() == 1 && TestWindow::aWindowManagerRuns(display)))
        return {};

    const QRect whole = screens.value(1, screens.first()).geometry;
    const std::vector<quint32> area = TestWindow::numbers(display.connection, display.root, "_NET_WORKAREA");
    const QRect free = area.size() >= 4 ? whole.intersected(QRect(static_cast<int>(area[0]), static_cast<int>(area[1]), static_cast<int>(area[2]), static_cast<int>(area[3]))) : whole;
    return QuietScreen{ whole, free.isEmpty() ? whole : free };
}

#define SKIP_WITHOUT_A_QUIET_SCREEN(screen)                  \
    const std::optional<QuietScreen> screen = quietScreen(); \
    if (!screen)                                             \
    QSKIP("the windows this opens go on a second screen, out of the way, or on a display nobody works at, and this X11 display is the only screen of somebody's desktop")

TestWindow::Look windowLook(const QSize &inside, const QPoint &at, const Rgb &colour = SHARED_COLOUR, bool framed = true)
{
    TestWindow::Look look;
    look.inside = inside;
    look.at = at;
    look.colour = static_cast<quint32>(colour.red << 16 | colour.green << 8 | colour.blue);
    look.framed = framed;
    look.outOfTheWay = true;
    look.animated = true;
    return look;
}

QRgb onTheScreen(const QuietScreen &screen, const QPoint &at)
{
    for (const Core::Video::CaptureSource &source : Core::Video::X11Capture::screens()) {
        if (source.geometry == screen.whole)
            return Core::Video::X11Capture::thumbnail(source, screen.whole.size()).pixel(at - screen.whole.topLeft());
    }
    return 0;
}

bool isColour(QRgb pixel, const Rgb &colour)
{
    return qRed(pixel) == colour.red && qGreen(pixel) == colour.green && qBlue(pixel) == colour.blue;
}
#endif

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
constexpr QSize STAND_IN_SCREEN(1920, 1080);
constexpr QSize STAND_IN_WINDOW(640, 480);
const char NO_PIPEWIRE[] = "PipeWire is not running here to hand the tests' own pictures over";
const char NO_CARD_BUFFERS[] = "this machine does not let a program make graphics card buffers of its own (/dev/udmabuf), so the tests have none to hand over";

class StandInDesktop
{
public:
    explicit StandInDesktop(const QSize &size, bool buffersOnTheCard = false)
        : stream(std::make_unique<PipeWireTestSource>(size, paint, SPA_VIDEO_FORMAT_BGRx, buffersOnTheCard)), sending([this] { send(); })
    {
        const quint32 node = stream->node();
        if (node != PW_ID_ANY)
            handedOver = Core::Video::PortalCapture::adopt(node, size);
    }

    ~StandInDesktop()
    {
        quitting = true;
        sending.join();
        stream.reset();
        Core::Video::PortalCapture::release(handedOver.id);
    }

    [[nodiscard]] bool isOpen() const { return !handedOver.id.isEmpty(); }
    [[nodiscard]] const Core::Video::CaptureSource &source() const { return handedOver; }
    void setAnimated(bool on) { animated = on; }

    void resize(const QSize &size)
    {
        std::lock_guard lock(guard);
        stream->resize(size);
    }

    void close()
    {
        std::lock_guard lock(guard);
        stream.reset();
    }

private:
    static void paint(void *into, const QSize &size, int stride, quint32 number, bool again)
    {
        constexpr int SQUARE = 16;
        constexpr quint32 COLOUR = 0xff000000u | (PATCH_RED << 16) | (PATCH_GREEN << 8) | PATCH_BLUE;
        for (int y = again ? size.height() - 2 * SQUARE : 0; y < size.height(); y++) {
            auto *row = reinterpret_cast<quint32 *>(static_cast<uint8_t *>(into) + static_cast<qsizetype>(y) * stride);
            const bool acrossTheSquare = y >= size.height() - 2 * SQUARE && y < size.height() - SQUARE;
            for (int x = 0; x < (again ? 2 * SQUARE : size.width()); x++)
                row[x] = acrossTheSquare && x >= SQUARE && x < 2 * SQUARE ? 0xff000000u | ((number * 16) & 0xffu) * 0x010101u : COLOUR;
        }
    }

    void send()
    {
        quint32 number = 0;
        while (!quitting) {
            {
                std::lock_guard lock(guard);
                if (stream && animated)
                    stream->send(++number);
            }
            QThread::msleep(16);
        }
    }

    std::mutex guard;
    std::unique_ptr<PipeWireTestSource> stream;
    Core::Video::CaptureSource handedOver;
    std::atomic<bool> animated{ true };
    std::atomic<bool> quitting{ false };
    std::thread sending;
};

class Heard
{
public:
    Heard()
    {
        said().clear();
        before() = qInstallMessageHandler(keep);
    }
    ~Heard() { qInstallMessageHandler(before()); }

    [[nodiscard]] static bool heard(const QString &part)
    {
        QMutexLocker lock(&mutex());
        return said().join(QLatin1Char('\n')).contains(part);
    }

private:
    static QMutex &mutex()
    {
        static QMutex mutex;
        return mutex;
    }
    static QStringList &said()
    {
        static QStringList said;
        return said;
    }
    static QtMessageHandler &before()
    {
        static QtMessageHandler before = nullptr;
        return before;
    }
    static void keep(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        {
            QMutexLocker lock(&mutex());
            said().append(message);
        }
        before()(type, context, message);
    }
};

QStringList othersOnTheStreamsCapturedHere()
{
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"), QStringList());
    if (!dump.waitForFinished(5000))
        return {};
    const QJsonArray objects = QJsonDocument::fromJson(dump.readAllStandardOutput()).array();

    QSet<int> ownClients;
    QHash<int, QJsonObject> nodes;
    for (const QJsonValue &object : objects) {
        const QJsonObject properties = object[QLatin1String("info")][QLatin1String("props")].toObject();
        if (object[QLatin1String("type")].toString() == QLatin1String("PipeWire:Interface:Client") && properties[QLatin1String("application.process.id")].toInt() == QCoreApplication::applicationPid())
            ownClients.insert(object[QLatin1String("id")].toInt());
        if (object[QLatin1String("type")].toString() == QLatin1String("PipeWire:Interface:Node"))
            nodes.insert(object[QLatin1String("id")].toInt(), properties);
    }
    const auto isOwn = [&](int node) { return ownClients.contains(nodes.value(node)[QLatin1String("client.id")].toInt()); };

    QSet<int> capturedHere;
    QList<std::pair<int, int>> links;
    for (const QJsonValue &object : objects) {
        if (object[QLatin1String("type")].toString() != QLatin1String("PipeWire:Interface:Link"))
            continue;
        const QJsonObject link = object[QLatin1String("info")].toObject();
        links.append({ link[QLatin1String("output-node-id")].toInt(), link[QLatin1String("input-node-id")].toInt() });
        if (isOwn(links.last().second) && !isOwn(links.last().first))
            capturedHere.insert(links.last().first);
    }
    QStringList others;
    for (const auto &[from, to] : links) {
        if (capturedHere.contains(from) && !isOwn(to))
            others.append(nodes.value(to)[QLatin1String("node.name")].toString());
    }
    return others;
}
#endif

} // namespace

Q_DECLARE_METATYPE(EncoderCase)

#define SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase)                                                     \
    if (!(encoderCase).encoderName.isEmpty() && !Core::Media::videoEncoderWorks((encoderCase).encoderName)) \
    QSKIP("this encoder does not open on this machine, or holds frames back")

class TestVideoEncode : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();

    void aSourceIsFittedInsideTheSendSize();
    void theProbeGivesTheSameAnswerEveryTime();
    void encodableCodecsAreOfferedForSending();
    void av1FromAmfIsCodedInWholeBlocks();
    void aCodecTurnedOffIsNotSentWith();
    void av1FromVaapiIsAskedWhatSizesItCodes();
    void blackColumnsAreNotAcceptableAndAFewRowsAre();
    void av1IsOfferedOnlyWhereItAddsNoBlack();
    void theProbeTurnsDownWhatCannotKeepUp_data() { encoderCases(); }
    void theProbeTurnsDownWhatCannotKeepUp();

    void everyPictureComesOutAtOnceAndDecodes_data() { encoderCases(); }
    void everyPictureComesOutAtOnceAndDecodes();
    void aChangedBitrateIsFollowed_data() { encoderCases(); }
    void aChangedBitrateIsFollowed();
    void smallMovesOfTheRateDoNotStartAnEncoderOver_data() { encoderCases(); }
    void smallMovesOfTheRateDoNotStartAnEncoderOver();
    void aStillPictureIsNotFilledUpToTheRate_data();
    void aStillPictureIsNotFilledUpToTheRate();
    void aChangedPictureSizeStartsANewStream_data() { encoderCases(); }
    void aChangedPictureSizeStartsANewStream();
    void aFrameIsMadeWholeWhateverItsEncoderLeftOut();
    void noKeyFrameComesUnasked_data() { encoderCases(); }
    void noKeyFrameComesUnasked();
    void theStreamSaysWhatColoursItIsIn_data();
    void theStreamSaysWhatColoursItIsIn();
    void aSizeIsCodedAsItIsSaidToBe_data() { encoderCases(); }
    void aSizeIsCodedAsItIsSaidToBe();

    void picturesOnTheGraphicsCardAreEncodedThere_data();
    void picturesOnTheGraphicsCardAreEncodedThere();
    void theCardScalesAndConvertsPictures_data();
    void theCardScalesAndConvertsPictures();
    void picturesScaledOnTheCardAreEncodedThere_data();
    void picturesScaledOnTheCardAreEncodedThere();

    void screensAreListedWithThumbnails();
    void thePipelineSharesTheScreen_data();
    void thePipelineSharesTheScreen();
    void aScaledScreenIsHandedOverAsItsEncoderTakesIt_data();
    void aScaledScreenIsHandedOverAsItsEncoderTakesIt();
    void aScreenThatIsNotThereFails();
    void aPipeWireStreamThatIsNotThereFails();
    void picturesTheDesktopLeavesOnTheCardAreSharedFromThere_data();
    void picturesTheDesktopLeavesOnTheCardAreSharedFromThere();
    void theRealScreenIsSharedOnTheCardAndInMemory();

    void theCaptureMethodIsRemembered();
    void aWindowAboveCoversTheOneBelow();
    void windowsAreListedWithoutThisProgramsOwn();
    void aWindowHasAThumbnail_data();
    void aWindowHasAThumbnail();
    void thePipelineSharesAWindow_data();
    void thePipelineSharesAWindow();
    void thePipelineHoldsBackWhatItsEncoderWouldPad();

private:
    static void encoderCases();

    QTemporaryDir settingsDirectory;
    QSet<QString> gaveNoAnswer;
};

Q_DECLARE_METATYPE(Core::Video::CaptureMethod)

void TestVideoEncode::encoderCases()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : encodersToTest(codec))
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
    }
}

void TestVideoEncode::init()
{
    Core::Video::setCaptureMethod(Core::Video::CaptureMethod::Auto);
}

void TestVideoEncode::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("AcheronTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TestVideoEncode"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
#ifdef Q_OS_WIN
    measureInDesktopPixels(true);
#endif

    QStringList codecs;
    for (VideoCodec codec : Core::Media::encodableVideoCodecs())
        codecs.append(videoCodecName(codec));
    qInfo() << "encodable codecs:" << codecs;
    for (VideoCodec codec : CODECS) {
        for (const QString &name : encodersToTest(codec)) {
            QElapsedTimer trial;
            trial.start();
            const bool works = Core::Media::videoEncoderWorks(name);
            // the probe abandons an encoder after ten seconds, leaving its thread inside it
            if (!works && trial.elapsed() > 9000)
                gaveNoAnswer.insert(name);
            qInfo().noquote() << videoCodecName(codec) << "encoder" << name << (works ? "works" : "is turned down by the probe");
        }
    }
}

void TestVideoEncode::aSourceIsFittedInsideTheSendSize()
{
    using Core::Video::encodeSizeFor;
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 720), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(2560, 1440), 1080), QSize(1920, 1080));
    QCOMPARE(encodeSizeFor(QSize(3840, 2160), std::nullopt), QSize(3840, 2160));
    QCOMPARE(encodeSizeFor(QSize(1280, 720), 1080), QSize(1280, 720));
    QCOMPARE(encodeSizeFor(QSize(1366, 768), 1080), QSize(1366, 768));
    QCOMPARE(encodeSizeFor(QSize(3440, 1440), 720), QSize(1280, 534));
    QCOMPARE(encodeSizeFor(QSize(1440, 2560), 720), QSize(404, 720));
    QCOMPARE(encodeSizeFor(QSize(1920, 1200), 720), QSize(1152, 720));
    QCOMPARE(encodeSizeFor(QSize(1365, 767), std::nullopt), QSize(1364, 766));
    QCOMPARE(encodeSizeFor(QSize(1920, 1080), 480), QSize(852, 480));

#ifdef Q_OS_WIN
    if (!Core::Video::screenCaptureAvailable())
        QSKIP("this build cannot capture a screen");
    Core::Video::CaptureSource screen;
    screen.geometry = QRect(1920, 0, 2560, 1440);
    QCOMPARE(Core::Video::encodeSizeOf(screen, 1080), QSize(1920, 1080));
    QCOMPARE(Core::Video::encodeSizeOf(screen, std::nullopt), QSize(2560, 1440));
    QCOMPARE(Core::Video::encodeSizeOf(Core::Video::CaptureSource(), 720), QSize());
    Core::Video::CaptureSource noWindow;
    noWindow.kind = Core::Video::CaptureSource::Kind::Window;
    noWindow.id = QStringLiteral("window:1");
    QCOMPARE(Core::Video::encodeSizeOf(noWindow, 720), QSize());
#endif
}

void TestVideoEncode::theProbeGivesTheSameAnswerEveryTime()
{
    const QList<VideoCodec> first = Core::Media::encodableVideoCodecs();
    for (int i = 0; i < 3; i++)
        QVERIFY(Core::Media::encodableVideoCodecs() == first);

    QList<VideoCodec> expected;
    for (VideoCodec codec : CODECS) {
        if (first.contains(codec))
            expected.append(codec);
    }
    QVERIFY(first == expected);

    for (VideoCodec codec : CODECS) {
        const QList<QString> names = Core::Media::videoEncoderNames(codec);
        const bool oneWorks = std::any_of(names.begin(), names.end(), [](const QString &name) { return Core::Media::videoEncoderWorks(name); });
        QCOMPARE(first.contains(codec), oneWorks);

        EncodeRun run({ codec, {} }, WIDTH, HEIGHT, BITRATE);
        QCOMPARE(run.encoder.hasEncoder(), oneWorks);
        QCOMPARE(run.encodeNext(), oneWorks);

        const QString chosen = Core::Media::videoEncoderFor(codec);
        const auto firstThatWorks = std::find_if(names.begin(), names.end(), [](const QString &name) { return Core::Media::videoEncoderWorks(name); });
        QCOMPARE(chosen, firstThatWorks == names.end() ? QString() : *firstThatWorks);
        if (oneWorks)
            QVERIFY2(run.encoder.stats().encoderName.startsWith(chosen), qPrintable(run.encoder.stats().encoderName));
    }
    EncodeRun unsupported({ VideoCodec::Unknown, {} }, WIDTH, HEIGHT, BITRATE);
    QVERIFY(!unsupported.encodeNext());

    for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265 }) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            if (!Core::Media::videoEncoderWorks(name))
                continue;
            EncodeRun run({ codec, name }, WIDTH, HEIGHT, BITRATE);
            QVERIFY(run.encodeNext());
            QVERIFY2(run.encoder.stats().hardware, qPrintable(name));
        }
    }
}

void TestVideoEncode::encodableCodecsAreOfferedForSending()
{
    const QList<VideoCodec> decodable = Core::Media::decodableVideoCodecs();
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const QList<Codec> offered = Core::Media::offeredVideoCodecs(encodable);
    QCOMPARE(offered.size(), decodable.size());
    for (int i = 0; i < offered.size(); i++) {
        QCOMPARE(offered[i].name.get(), videoCodecName(decodable[i]));
        QCOMPARE(offered[i].decode.get(), true);
        QCOMPARE(offered[i].encode.get(), encodable.contains(decodable[i]));
        QCOMPARE(offered[i].payloadType.get(), 101 + 2 * i);
    }
    for (const Codec &codec : Core::Media::offeredVideoCodecs())
        QCOMPARE(codec.encode.get(), false);
}

void TestVideoEncode::aCodecTurnedOffIsNotSentWith()
{
    using namespace Core::Media;
    const QVariant before = QSettings().value(QStringLiteral("video/send_codecs_off"));
    const auto restore = qScopeGuard([&before] { QSettings().setValue(QStringLiteral("video/send_codecs_off"), before); });
    QSettings().remove(QStringLiteral("video/send_codecs_off"));

    const QList<VideoCodec> encodable = { VideoCodec::AV1, VideoCodec::H264, VideoCodec::VP8 };
    QCOMPARE(allowedSendCodecs(encodable), encodable);

    setSendCodecAllowed(VideoCodec::AV1, false);
    setSendCodecAllowed(VideoCodec::H265, false);
    QVERIFY(!sendCodecAllowed(VideoCodec::AV1));
    QVERIFY(sendCodecAllowed(VideoCodec::H264));
    QCOMPARE(allowedSendCodecs(encodable), QList<VideoCodec>({ VideoCodec::H264, VideoCodec::VP8 }));

    setSendCodecAllowed(VideoCodec::AV1, false);
    setSendCodecAllowed(VideoCodec::AV1, true);
    QVERIFY(sendCodecAllowed(VideoCodec::AV1));
    QCOMPARE(allowedSendCodecs(encodable), encodable);
    QVERIFY(!sendCodecAllowed(VideoCodec::H265));
}

void TestVideoEncode::av1FromAmfIsCodedInWholeBlocks()
{
    using Core::Media::codedSize;
    const QString amf = QStringLiteral("av1_amf");
    QCOMPARE(codedSize(amf, QSize(1920, 1080)), QSize(1920, 1082));
    QCOMPARE(codedSize(amf, QSize(640, 360)), QSize(640, 362));
    QCOMPARE(codedSize(amf, QSize(960, 540)), QSize(960, 544));
    QCOMPARE(codedSize(amf, QSize(1000, 700)), QSize(1024, 704));
    QCOMPARE(codedSize(amf, QSize(1280, 720)), QSize(1280, 720));
    QCOMPARE(codedSize(amf, QSize(2560, 1440)), QSize(2560, 1440));
    QCOMPARE(codedSize(amf, QSize(1452, 752)), QSize(1472, 752));

    QStringList others = { QString(), QStringLiteral("av1_nvenc"), QStringLiteral("av1_qsv"), QStringLiteral("av1_mf hardware") };
    for (VideoCodec codec : CODECS)
        others.append(Core::Media::videoEncoderNames(codec));
    others.removeAll(amf);
    others.removeAll(QStringLiteral("av1_vaapi"));
    QCOMPARE(others.contains(QStringLiteral("libvpx")), avcodec_find_encoder_by_name("libvpx") != nullptr);
    for (const QString &name : std::as_const(others)) {
        for (const QSize &size : { QSize(1920, 1080), QSize(640, 360), QSize(1000, 700), QSize(1452, 752) })
            QVERIFY2(codedSize(name, size) == size, qPrintable(name));
    }
}

void TestVideoEncode::blackColumnsAreNotAcceptableAndAFewRowsAre()
{
    using Core::Media::paddingIsAcceptable;
    QVERIFY(paddingIsAcceptable(QSize(1280, 720), QSize(1280, 720)));
    QVERIFY(paddingIsAcceptable(QSize(1280, 720), QSize(1280, 728)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1280, 730)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1282, 720)));
    QVERIFY(!paddingIsAcceptable(QSize(1280, 720), QSize(1344, 722)));

    const auto accepted = [](const QString &encoder, const QSize &size) { return paddingIsAcceptable(size, Core::Media::codedSize(encoder, size)); };
    const QString amf = QStringLiteral("av1_amf");
    QVERIFY(accepted(amf, QSize(1920, 1080)));
    QVERIFY(accepted(amf, QSize(640, 360)));
    QVERIFY(accepted(amf, QSize(960, 540)));
    QVERIFY(accepted(amf, QSize(1280, 720)));
    QVERIFY(accepted(amf, QSize(2560, 1440)));
    QVERIFY(!accepted(amf, QSize(1000, 700)));
    QVERIFY(!accepted(amf, QSize(1452, 752)));
    QVERIFY(!accepted(amf, QSize(1280, 722)));

    for (const QString &exact : { QStringLiteral("av1_nvenc"), QStringLiteral("av1_qsv"), QStringLiteral("hevc_amf"), QStringLiteral("libvpx") }) {
        for (const QSize &size : { QSize(1920, 1080), QSize(1000, 700), QSize(1452, 752), QSize(1280, 722) })
            QVERIFY2(accepted(exact, size), qPrintable(exact));
    }
}

void TestVideoEncode::av1IsOfferedOnlyWhereItAddsNoBlack()
{
    using Core::Media::cameraCodecs;
    using Core::Media::shareCodecs;
    const QString amf = QStringLiteral("av1_amf");
    const QString nvenc = QStringLiteral("av1_nvenc");
    const QList<VideoCodec> all = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };
    const QList<VideoCodec> withoutAv1 = { VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

    QVERIFY(shareCodecs(all, QSize(1920, 1080), amf) == all);
    QVERIFY(shareCodecs(all, QSize(1280, 720), amf) == all);
    QVERIFY(shareCodecs(all, QSize(1452, 752), amf) == withoutAv1);
    QVERIFY(shareCodecs(all, QSize(1000, 700), amf) == withoutAv1);
    QVERIFY(shareCodecs(all, QSize(1452, 752), nvenc) == all);
    QVERIFY(shareCodecs(withoutAv1, QSize(1452, 752), amf) == withoutAv1);
    QVERIFY(shareCodecs(shareCodecs(all, QSize(1452, 752), amf), QSize(1920, 1080), amf) == withoutAv1);

    QVERIFY(cameraCodecs(all, amf) == withoutAv1);
    QVERIFY(cameraCodecs(all, nvenc) == all);
    QVERIFY(cameraCodecs(all, QStringLiteral("av1_qsv")) == all);
    QVERIFY(cameraCodecs(withoutAv1, amf) == withoutAv1);
    QVERIFY(cameraCodecs({ VideoCodec::VP8 }, amf) == QList<VideoCodec>{ VideoCodec::VP8 });
}

// Microsoft's software H264 encoder runs sixteen pictures behind, and a Radeon's AV1 encoder through
// Media Foundation never answers
void TestVideoEncode::theProbeTurnsDownWhatCannotKeepUp()
{
    QFETCH(EncoderCase, encoderCase);
    if (gaveNoAnswer.contains(encoderCase.encoderName))
        QSKIP("the probe gave this one up, and a second thread is not left inside it");
    constexpr int PICTURES = 30;
    constexpr int SETTLING = 10;
    constexpr int PROBE_BITRATE = 1'000'000;
    const int answersWithinMs = Core::Media::videoEncoderWorks(encoderCase.encoderName) ? 15000 : 3000;

    struct Seen
    {
        bool opens = true;
        int frames = 0;
        int sent = 0;
        int withTheirPicture = 0;
        bool startsWithAKeyFrame = false;
        double rate = 0;
    };
    const auto seen = std::make_shared<std::promise<Seen>>();
    std::future<Seen> finished = seen->get_future();
    std::thread([seen, encoderCase] {
        Seen result;
        EncodeRun run(encoderCase, WIDTH, HEIGHT, PROBE_BITRATE);
        size_t settled = 0;
        for (int index = 0; index < PICTURES && result.opens; index++) {
            if (index == SETTLING)
                settled = run.frames.size();
            result.opens = run.encodeNext();
        }
        result.frames = static_cast<int>(run.frames.size());
        result.sent = run.sent;
        result.withTheirPicture = static_cast<int>(std::count(run.producedItsOwnFrame.begin(), run.producedItsOwnFrame.end(), true));
        result.startsWithAKeyFrame = !run.frames.empty() && run.frames[0].keyframe;
        result.rate = run.bitsPerSecond(settled, run.frames.size(), PICTURES - SETTLING);
        seen->set_value(result);
    }).detach();

    if (finished.wait_for(std::chrono::milliseconds(answersWithinMs)) != std::future_status::ready) {
        qInfo().noquote() << encoderCase.encoderName << "never answers";
        QVERIFY(!Core::Media::videoEncoderWorks(encoderCase.encoderName));
        return;
    }
    const Seen result = finished.get();
    qInfo().noquote() << encoderCase.encoderName << (result.opens ? "opens;" : "does not open;") << result.frames << "frames from" << result.sent << "pictures," << result.withTheirPicture << "of them with the picture's own call; asked" << PROBE_BITRATE << "made" << qRound(result.rate);

    const bool keepsUp = result.opens && result.withTheirPicture == PICTURES && result.startsWithAKeyFrame;
    const bool holdsTheRate = result.rate > PROBE_BITRATE * 0.25 && result.rate < PROBE_BITRATE * 2.0;
    QCOMPARE(Core::Media::videoEncoderWorks(encoderCase.encoderName), keepsUp && holdsTheRate);
}

void TestVideoEncode::everyPictureComesOutAtOnceAndDecodes()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 75;
    const std::vector<int> forced = { 20, 21, 47 };

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    for (int index = 0; index < PICTURES; index++) {
        if (std::find(forced.begin(), forced.end(), index) != forced.end())
            run.encoder.requestKeyframe();
        QVERIFY2(run.encodeNext(), qPrintable(QStringLiteral("picture %1 could not be encoded").arg(index)));
    }
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "hardware:" << run.encoder.stats().hardware;
    qInfo().noquote() << "the first key frame is made of units" << unitTypes(encoderCase.codec, run.frames[0]) << "and the frame after it of" << unitTypes(encoderCase.codec, run.frames[1]);
    if (encoderCase.codec != VideoCodec::VP8) {
        const int profile = profileOf(encoderCase.codec, run.frames[0].data);
        qInfo() << "profile:" << profile;
        QVERIFY(encoderCase.codec == VideoCodec::H264 ? profile == 77 || profile == 100 : profile == (encoderCase.codec == VideoCodec::H265 ? 1 : 0));
    }

    // x264 makes a slice per thread; a picture this small gets one
    if (run.encoder.stats().encoderName == QLatin1String("libx264"))
        QCOMPARE(nalTypes(encoderCase.codec, run.frames[1].data).size(), size_t(1));

    QCOMPARE(run.frames.size(), size_t(PICTURES));
    for (int index = 0; index < PICTURES; index++)
        QVERIFY2(run.producedItsOwnFrame[static_cast<size_t>(index)], qPrintable(QStringLiteral("picture %1 did not come out with its own call").arg(index)));
    QCOMPARE(run.encoder.stats().opened, quint64(1));
    QCOMPARE(run.encoder.stats().encoded, quint64(PICTURES));

    for (int index = 0; index < PICTURES; index++) {
        const bool expected = index == 0 || std::find(forced.begin(), forced.end(), index) != forced.end();
        QVERIFY2(run.frames[static_cast<size_t>(index)].keyframe == expected, qPrintable(QStringLiteral("frame %1: key frame %2, expected %3").arg(index).arg(run.frames[static_cast<size_t>(index)].keyframe).arg(expected)));
    }
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));

    const Decoded whole = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(whole.errors, quint64(0));
    QCOMPARE(whole.dropped, quint64(0));
    QCOMPARE(whole.pictures, PICTURES);
    QVERIFY2(isThePictureSent(encoderCase.codec, whole, QSize(WIDTH, HEIGHT)), qPrintable(QStringLiteral("%1x%2").arg(whole.width).arg(whole.height)));
    QVERIFY(whole.format == AV_PIX_FMT_YUV420P || whole.format == AV_PIX_FMT_YUVJ420P);

    for (size_t index = 0; index < run.frames.size(); index++) {
        if (!run.frames[index].keyframe)
            continue;
        const Decoded alone = decodeAll(encoderCase.codec, std::vector<EncodedVideoFrame>{ run.frames[index] });
        QCOMPARE(alone.errors, quint64(0));
        QCOMPARE(alone.pictures, 1);
        QVERIFY(isThePictureSent(encoderCase.codec, alone, QSize(WIDTH, HEIGHT)));

        const std::vector<EncodedVideoFrame> fromHere(run.frames.begin() + static_cast<std::ptrdiff_t>(index), run.frames.end());
        const Decoded rest = decodeAll(encoderCase.codec, fromHere);
        QCOMPARE(rest.errors, quint64(0));
        QCOMPARE(rest.pictures, static_cast<int>(fromHere.size()));
    }
}

void TestVideoEncode::aChangedBitrateIsFollowed()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES_PER_RATE = 90;
    constexpr int SETTLING = 30;
    constexpr int LOWER_BITRATE = 500'000;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    size_t settledBefore = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledBefore = run.frames.size();
        QVERIFY(run.encodeNext());
    }
    const size_t atChange = run.frames.size();

    run.encoder.setBitrate(LOWER_BITRATE);
    size_t settledAfter = 0;
    for (int index = 0; index < PICTURES_PER_RATE; index++) {
        if (index == SETTLING)
            settledAfter = run.frames.size();
        QVERIFY(run.encodeNext());
    }

    const double before = run.bitsPerSecond(settledBefore, atChange, PICTURES_PER_RATE - SETTLING);
    const double after = run.bitsPerSecond(settledAfter, run.frames.size(), PICTURES_PER_RATE - SETTLING);
    qInfo() << "encoder:" << run.encoder.stats().encoderName << "asked" << BITRATE << "made" << qRound(before) << "; asked" << LOWER_BITRATE << "made" << qRound(after) << "; opened" << run.encoder.stats().opened << "time(s);" << run.frames.size() << "frames from" << run.sent << "pictures";

    QVERIFY2(before > BITRATE * 0.7 && before < BITRATE * 1.3, qPrintable(QString::number(before)));
    QVERIFY2(after > LOWER_BITRATE * 0.7 && after < LOWER_BITRATE * 1.3, qPrintable(QString::number(after)));

    if (run.encoder.stats().opened > 1)
        QVERIFY(run.frames[atChange].keyframe);
    if (run.encoder.stats().encoderName == QLatin1String("libx264"))
        QVERIFY(run.encoder.stats().opened == 1 && !run.frames[atChange].keyframe);
    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));
    const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(run.frames.size()));
}

void TestVideoEncode::smallMovesOfTheRateDoNotStartAnEncoderOver()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES_PER_RATE = 6;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    const auto openedAt = [&run](int bitrate) {
        run.encoder.setBitrate(bitrate);
        for (int index = 0; index < PICTURES_PER_RATE; index++) {
            if (!run.encodeNext())
                return quint64(0);
        }
        return static_cast<quint64>(run.encoder.stats().opened);
    };

    const quint64 atFirst = openedAt(BITRATE);
    QCOMPARE(atFirst, quint64(1));

    const quint64 lowered = openedAt(BITRATE * 95 / 100);
    QVERIFY(lowered == atFirst || lowered == atFirst + 1);
    const bool startsOver = lowered > atFirst;
    QCOMPARE(openedAt(BITRATE * 90 / 100), lowered);
    QCOMPARE(openedAt(BITRATE * 88 / 100), lowered);
    QCOMPARE(openedAt(BITRATE), lowered);

    QCOMPARE(openedAt(BITRATE * 80 / 100), lowered + (startsOver ? 1 : 0));
    QCOMPARE(openedAt(BITRATE * 120 / 100), lowered + (startsOver ? 2 : 0));

    for (const EncodedVideoFrame &frame : run.frames)
        QVERIFY(isWellFormed(encoderCase.codec, frame));
    const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
    QCOMPARE(decoded.errors, quint64(0));
}

void TestVideoEncode::aStillPictureIsNotFilledUpToTheRate_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    QTest::addColumn<EncoderCase>("encoderCase");
    bool any = false;
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            if (!name.endsWith(QLatin1String("_vaapi")))
                continue;
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
            any = true;
        }
    }
    if (!any)
        QSKIP("none of Mesa's encoders is here");
}

// Mesa pads every constant bit rate frame up to the rate, so it is asked for VBR instead
void TestVideoEncode::aStillPictureIsNotFilledUpToTheRate()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 90;
    constexpr int SETTLING = 30;

    EncodeRun run(encoderCase, 1280, 720, BITRATE);
    run.paintNext();
    size_t settled = 0;
    for (int index = 0; index < PICTURES; index++) {
        if (index == SETTLING)
            settled = run.frames.size();
        QVERIFY(run.encodeNext(false));
    }
    const double rate = run.bitsPerSecond(settled, run.frames.size(), PICTURES - SETTLING);
    QVERIFY2(rate < BITRATE / 2, qPrintable(QString::number(rate)));
}

void TestVideoEncode::aChangedPictureSizeStartsANewStream()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 10;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    QVERIFY(run.encodeNext());

    AVFrame *larger = allocatePicture(run.encoder.softwareFormat(), 960, 540);
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        paint(larger, index);
        QVERIFY(run.encoder.encode(larger, (index + 1) * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
    }
    av_frame_free(&larger);

    QCOMPARE(frames.size(), size_t(PICTURES));
    QVERIFY(frames[0].keyframe);
    QVERIFY(!frames[1].keyframe);
    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, PICTURES);
    QVERIFY2(isThePictureSent(encoderCase.codec, decoded, QSize(960, 540)), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
}

namespace {

std::vector<uint8_t> joined(const std::vector<std::vector<uint8_t>> &parts)
{
    std::vector<uint8_t> whole;
    for (const std::vector<uint8_t> &part : parts)
        whole.insert(whole.end(), part.begin(), part.end());
    return whole;
}

std::vector<uint8_t> annexB(const std::vector<std::vector<uint8_t>> &nals)
{
    std::vector<uint8_t> frame;
    for (const std::vector<uint8_t> &nal : nals) {
        frame.insert(frame.end(), { 0, 0, 0, 1 });
        frame.insert(frame.end(), nal.begin(), nal.end());
    }
    return frame;
}

std::vector<uint8_t> obu(int type, const std::vector<uint8_t> &payload, bool saysItsSize = true)
{
    std::vector<uint8_t> whole = { static_cast<uint8_t>((type << 3) | (saysItsSize ? 0x02 : 0)) };
    for (size_t size = payload.size(); saysItsSize; size >>= 7) {
        whole.push_back(static_cast<uint8_t>((size & 0x7F) | (size > 0x7F ? 0x80 : 0)));
        if (size <= 0x7F)
            break;
    }
    whole.insert(whole.end(), payload.begin(), payload.end());
    return whole;
}

std::vector<uint8_t> sequenceHeader(const QSize &size, int level, bool displayDelay = false, bool timingInfo = false)
{
    std::vector<uint8_t> header;
    int written = 0;
    const auto put = [&](uint32_t value, int count) {
        for (int bit = count - 1; bit >= 0; bit--, written++) {
            if (written % 8 == 0)
                header.push_back(0);
            header.back() |= static_cast<uint8_t>(((value >> bit) & 1) << (7 - written % 8));
        }
    };
    put(0, 5);
    put(timingInfo, 1);
    put(displayDelay, 1);
    put(0, 5 + 12);
    put(static_cast<uint32_t>(level), 5);
    if (level > 7)
        put(0, 1);
    if (displayDelay)
        put(0b10011, 5);
    put(15, 4);
    put(15, 4);
    put(static_cast<uint32_t>(size.width() - 1), 16);
    put(static_cast<uint32_t>(size.height() - 1), 16);
    return header;
}

struct Finished
{
    bool whole = false;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

Finished finish(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &data)
{
    EncodedVideoFrame frame;
    frame.data = data;
    Finished finished;
    finished.whole = finisher.finish(frame);
    finished.keyframe = frame.keyframe;
    finished.data = frame.data;
    return finished;
}

void remember(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &extradata)
{
    finisher.rememberParameterSets(extradata.data(), extradata.size());
}

void stampPatch(AVFrame *picture, const Yuv &colour)
{
    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = picture->height * 3 / 8; y < picture->height * 5 / 8; y++) {
        for (int x = picture->width * 3 / 8; x < picture->width * 5 / 8; x++) {
            picture->data[0][y * picture->linesize[0] + x] = static_cast<uint8_t>(std::lround(colour.y));
            if (interleaved) {
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2 + 1] = static_cast<uint8_t>(std::lround(colour.v));
            } else {
                picture->data[1][(y / 2) * picture->linesize[1] + x / 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[2][(y / 2) * picture->linesize[2] + x / 2] = static_cast<uint8_t>(std::lround(colour.v));
            }
        }
    }
}

} // namespace

void TestVideoEncode::av1FromVaapiIsAskedWhatSizesItCodes()
{
    using Core::Media::av1CodedSize;
    using Core::Media::codesEverySizeAsItIs;
    const std::vector<uint8_t> delimiter = obu(OBU_TEMPORAL_DELIMITER, {});
    const std::vector<uint8_t> picture = obu(OBU_FRAME, { 0x10, 0x00, 0x55 });

    QCOMPARE(av1CodedSize(joined({ obu(OBU_SEQUENCE_HEADER, sequenceHeader(QSize(640, 360), 5)), picture })), QSize(640, 360));
    QCOMPARE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequenceHeader(QSize(1920, 1082), 13)), picture })), QSize(1920, 1082));
    QCOMPARE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequenceHeader(QSize(364, 196), 0, true)), picture })), QSize(364, 196));
    QCOMPARE(av1CodedSize(obu(OBU_SEQUENCE_HEADER, sequenceHeader(QSize(8192, 4352), 19, true), false)), QSize(8192, 4352));

    std::vector<uint8_t> cutShort = sequenceHeader(QSize(640, 360), 5);
    cutShort.resize(5);
    QCOMPARE(av1CodedSize(joined({ delimiter, picture })), QSize());
    QCOMPARE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, cutShort), picture })), QSize());
    QCOMPARE(av1CodedSize(obu(OBU_SEQUENCE_HEADER, sequenceHeader(QSize(640, 360), 5, false, true))), QSize());
    QCOMPARE(av1CodedSize({ 0xB2, 0x01, 0x10 }), QSize());
    QCOMPARE(av1CodedSize({}), QSize());

    const QSize trial(640, 360);
    const QSize odd(364, 196);
    QVERIFY(codesEverySizeAsItIs({ { trial, trial }, { odd, odd } }));
    QVERIFY(!codesEverySizeAsItIs({ { trial, QSize(640, 362) }, { odd, QSize(384, 208) } }));
    QVERIFY(!codesEverySizeAsItIs({ { trial, trial }, { odd, QSize(368, 196) } }));
    QVERIFY(!codesEverySizeAsItIs({ { trial, QSize() }, { odd, QSize() } }));
    QVERIFY(!codesEverySizeAsItIs({ { trial, trial }, { odd, QSize() } }));
    QVERIFY(!codesEverySizeAsItIs({}));
}

void TestVideoEncode::aFrameIsMadeWholeWhateverItsEncoderLeftOut()
{
    using Core::Media::EncodedFrameFinisher;
    {
        const std::vector<uint8_t> sps = { 0x67, 0x64, 0x1F }, pps = { 0x68, 0xEE }, laterSps = { 0x67, 0x4D, 0x28 }, laterPps = { 0x68, 0xCE };
        const std::vector<uint8_t> delimiter = { 0x09, 0x10 }, idr = { 0x65, 0x88, 0x80 }, slice = { 0x41, 0x9A, 0x20 };
        EncodedFrameFinisher finisher(VideoCodec::H264);
        QVERIFY(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ sps, pps, idr }));

        finished = finish(finisher, annexB({ delimiter, slice }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == annexB({ delimiter, slice }));

        finished = finish(finisher, annexB({ laterSps, laterPps, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ laterSps, laterPps, idr }));
        QVERIFY(finish(finisher, annexB({ idr })).data == annexB({ laterSps, laterPps, idr }));

        QVERIFY(!finish(finisher, { 0, 0, 0, 3, 0x65, 0x88, 0x80 }).whole);
        QVERIFY(!finish(finisher, {}).whole);
    }
    {
        const std::vector<uint8_t> vps = { 0x40, 0x01, 0x0C }, sps = { 0x42, 0x01, 0x01 }, pps = { 0x44, 0x01, 0xC1 }, laterSps = { 0x42, 0x01, 0x02 }, laterPps = { 0x44, 0x01, 0xC2 };
        const std::vector<uint8_t> delimiter = { 0x46, 0x01, 0x50 }, sei = { 0x4E, 0x01, 0x05 }, idr = { 0x26, 0x01, 0xAF }, cleanRandomAccess = { 0x2A, 0x01, 0xAF }, trailing = { 0x02, 0x01, 0xD0 };
        EncodedFrameFinisher finisher(VideoCodec::H265);
        remember(finisher, annexB({ sps, pps }));
        QVERIFY(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ vps, sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, sei, idr }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ vps, sps, pps, sei, idr }));

        finished = finish(finisher, annexB({ cleanRandomAccess }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == annexB({ vps, sps, pps, cleanRandomAccess }));

        finished = finish(finisher, annexB({ delimiter, trailing }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == annexB({ delimiter, trailing }));

        QVERIFY(finish(finisher, annexB({ vps, sps, pps, idr })).data == annexB({ vps, sps, pps, idr }));
        QVERIFY(finish(finisher, annexB({ laterSps, laterPps, idr })).data == annexB({ vps, laterSps, laterPps, idr }));
    }
    {
        constexpr int TEMPORAL_DELIMITER = OBU_TEMPORAL_DELIMITER;
        constexpr int TILE_GROUP = 4;
        const std::vector<uint8_t> sequence = { 0x00, 0x00, 0x00, 0x42, 0xAB }, laterSequence = { 0x00, 0x00, 0x00, 0x6A, 0xAB };
        const std::vector<uint8_t> key = { 0x10, 0x00, 0x55 }, predicted = { 0x30, 0x11 }, shownAgain = { 0x90 };
        const std::vector<uint8_t> delimiter = obu(TEMPORAL_DELIMITER, {});
        EncodedFrameFinisher finisher(VideoCodec::AV1);
        QVERIFY(!finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) })).whole);

        remember(finisher, obu(OBU_SEQUENCE_HEADER, sequence));
        Finished finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted) }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME_HEADER, shownAgain) }));
        QVERIFY(finished.whole && !finished.keyframe);

        finished = finish(finisher, joined({ obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        QVERIFY(finish(finisher, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));

        std::vector<uint8_t> longKey(300, 0x5A);
        longKey[0] = key[0];
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, longKey, false) }));
        QVERIFY(finished.whole && finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, longKey) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted, false) }));
        QVERIFY(finished.whole && !finished.keyframe);
        QVERIFY(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));

        QVERIFY(!finish(finisher, { 0xB2, 0x01, 0x10 }).whole);
        QVERIFY(!finish(finisher, { 0x32, 0x09, 0x10 }).whole);
        QVERIFY(!finish(finisher, delimiter).whole);

        EncodedFrameFinisher fromConfiguration(VideoCodec::AV1);
        remember(fromConfiguration, joined({ { 0x81, 0x00, 0x0C, 0x00 }, obu(OBU_SEQUENCE_HEADER, sequence) }));
        QVERIFY(finish(fromConfiguration, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        const std::vector<uint8_t> stills = { 0x18, 0x00 };
        finished = finish(fromConfiguration, joined({ obu(OBU_SEQUENCE_HEADER, stills), obu(OBU_FRAME, predicted) }));
        QVERIFY(finished.whole && finished.keyframe);
    }
    {
        EncodedFrameFinisher finisher(VideoCodec::VP8);
        QVERIFY(finish(finisher, { 0x10, 0x02, 0x00 }).keyframe);
        Finished finished = finish(finisher, { 0x11, 0x02, 0x00 });
        QVERIFY(finished.whole && !finished.keyframe);
        EncodedFrameFinisher unknown;
        QVERIFY(!finish(unknown, { 0x10, 0x02, 0x00 }).whole);
    }
}

// a hardware encoder left alone makes a key frame every few hundred pictures (a Radeon's every 240 or
// 300), which the other tests are too short to see
void TestVideoEncode::noKeyFrameComesUnasked()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 330;

    EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
    for (int index = 0; index < PICTURES; index++)
        QVERIFY(run.encodeNext());
    QVERIFY(run.frames[0].keyframe);
    QCOMPARE(static_cast<int>(std::count_if(run.frames.begin(), run.frames.end(), [](const EncodedVideoFrame &frame) { return frame.keyframe; })), 1);
    QCOMPARE(run.encoder.stats().opened, quint64(1));
}

void TestVideoEncode::theStreamSaysWhatColoursItIsIn_data()
{
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : encodersToTest(codec)) {
            QTest::newRow(qPrintable(name + QStringLiteral(", BT.709"))) << EncoderCase{ codec, name, AVCOL_SPC_BT709 };
            QTest::newRow(qPrintable(name + QStringLiteral(", BT.601"))) << EncoderCase{ codec, name, AVCOL_SPC_SMPTE170M };
        }
    }
}

// a stream that names no matrix is taken as BT.709 from 720 lines up and BT.601 below
void TestVideoEncode::theStreamSaysWhatColoursItIsIn()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 20;
    const auto sent = static_cast<AVColorSpace>(encoderCase.detail);
    const Yuv patch = patchInYuv(sent == AVCOL_SPC_BT709);

    VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
    AVFrame *picture = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
    const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        av_frame_make_writable(picture);
        paintYuv(picture, index);
        stampPatch(picture, patch);
        picture->colorspace = sent;
        picture->color_range = AVCOL_RANGE_MPEG;
        QVERIFY(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
    }

    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    qInfo().noquote() << QStringLiteral("%1: a patch of %2 %3 %4 decodes to %5 %6 %7; the stream was told matrix %8 and names matrix %9 and range %10").arg(encoder.stats().encoderName).arg(patch.y, 0, 'f', 0).arg(patch.u, 0, 'f', 0).arg(patch.v, 0, 'f', 0).arg(decoded.middle.y).arg(decoded.middle.u).arg(decoded.middle.v).arg(sent).arg(decoded.colorspace).arg(decoded.range);
    QVERIFY(distance(decoded.middle, patch) < 6);
    QVERIFY(decoded.range != AVCOL_RANGE_JPEG);
    if (encoderCase.codec == VideoCodec::VP8)
        return;
    if (sent == AVCOL_SPC_BT709)
        QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
    else
        QVERIFY(decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG || decoded.colorspace == AVCOL_SPC_UNSPECIFIED);
}

// a Radeon 7000 codes AV1 in 64x16 blocks, two rows more where eight are missing (1080, 360 lines), and
// sends the padding as picture
void TestVideoEncode::aSizeIsCodedAsItIsSaidToBe()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 5;

    for (const QSize &size : { QSize(1920, 1080), QSize(640, 360), QSize(1000, 700) }) {
        EncodeRun run(encoderCase, size.width(), size.height(), 3'000'000);
        for (int index = 0; index < PICTURES; index++)
            QVERIFY(run.encodeNext());
        const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, static_cast<int>(run.frames.size()));
        const QString outcome = QStringLiteral("%1x%2 as %3x%4").arg(size.width()).arg(size.height()).arg(decoded.width).arg(decoded.height);
        QVERIFY2(isThePictureSent(encoderCase.codec, decoded, size), qPrintable(outcome));
        QVERIFY2(QSize(decoded.width, decoded.height) == Core::Media::codedSize(encoderCase.encoderName, size), qPrintable(outcome));
    }
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec)) {
            QTest::newRow(qPrintable(name + QStringLiteral(", NV12"))) << EncoderCase{ codec, name, AV_PIX_FMT_NV12 };
            QTest::newRow(qPrintable(name + QStringLiteral(", BGRA"))) << EncoderCase{ codec, name, AV_PIX_FMT_BGRA };
        }
    }
}

void TestVideoEncode::picturesOnTheGraphicsCardAreEncodedThere()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    constexpr int PICTURES = 40;
    constexpr int FORCED = 20;
    const bool rgb = encoderCase.detail == AV_PIX_FMT_BGRA;

    {
        CardPictures pictures(static_cast<AVPixelFormat>(encoderCase.detail), WIDTH, HEIGHT, AVCOL_SPC_BT709, cardDevice(encoderCase.encoderName));
        if (!pictures.usable())
            QSKIP("the graphics card has no device that takes these pictures");

        VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
        AVFrame *fromMemory = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
        const auto freeFromMemory = qScopeGuard([&fromMemory] { av_frame_free(&fromMemory); });
        paint(fromMemory, 0);
        std::vector<EncodedVideoFrame> frames;

        if (!(rgb ? encoder.takesRgbCardFrames() : encoder.takesCardFrames())) {
            AVFrame *picture = pictures.next(0);
            QVERIFY(picture);
            QVERIFY(encoder.encode(picture, 0, frames) == VideoEncoder::Outcome::CardFramesRefused);
            QVERIFY(frames.empty());
            QVERIFY(encoder.encode(fromMemory, TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
            QCOMPARE(frames.size(), size_t(1));
            QVERIFY(frames[0].keyframe);
            return;
        }

        for (int index = 0; index < PICTURES; index++) {
            if (index == FORCED)
                encoder.requestKeyframe();
            AVFrame *picture = pictures.next(index);
            QVERIFY(picture);
            QVERIFY2(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded, qPrintable(QStringLiteral("picture %1 was not taken from the card").arg(index)));
            QCOMPARE(frames.size(), size_t(index + 1));
            QCOMPARE(frames.back().rtpTimestamp, uint32_t(index * TICKS_PER_FRAME));
        }
        qInfo() << "encoder:" << encoder.stats().encoderName << (rgb ? "BGRA" : "NV12") << "pictures," << encoder.stats().averageEncodeMs << "ms a picture";
        QVERIFY(encoder.stats().encoderName.endsWith(pictures.device));
        QCOMPARE(encoder.stats().opened, quint64(1));
        for (int index = 0; index < PICTURES; index++) {
            QCOMPARE(frames[static_cast<size_t>(index)].keyframe, index == 0 || index == FORCED);
            QVERIFY(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
        }

        const Decoded decoded = decodeAll(encoderCase.codec, frames);
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, PICTURES);
        QVERIFY(isThePictureSent(encoderCase.codec, decoded, QSize(WIDTH, HEIGHT)));
        if (rgb) {
            qInfo() << "the patch decodes to" << decoded.middle.y << decoded.middle.u << decoded.middle.v << ", off BT.709 by" << distance(decoded.middle, patchInYuv(true)) << "and off BT.601 by" << distance(decoded.middle, patchInYuv(false));
            QVERIFY(distance(decoded.middle, patchInYuv(true)) < 6);
            QVERIFY(distance(decoded.middle, patchInYuv(false)) > 12);
            QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
        }

        QVERIFY(encoder.encode(fromMemory, PICTURES * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
        QCOMPARE(frames.size(), size_t(PICTURES + 1));
        QVERIFY(frames.back().keyframe);
        QVERIFY(!encoder.stats().encoderName.endsWith(pictures.device));
    }
}

void TestVideoEncode::theCardScalesAndConvertsPictures_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    QTest::addColumn<bool>("bt601");
    QTest::newRow("to BT.709") << false;
    QTest::newRow("to BT.601") << true;
}

// video-range YUV of full-range RGB: Y = 16 + 219 (Kr R + Kg G + Kb B) / 255,
// U = 128 + 224 (B - Y') / (2 (1 - Kb)) / 255, V likewise from R; Kr, Kb = 0.2126, 0.0722 (BT.709) or
// 0.299, 0.114 (BT.601)
void TestVideoEncode::theCardScalesAndConvertsPictures()
{
    QFETCH(bool, bt601);
    using Core::Video::CardScaler;
    constexpr double CLOSE = 2.5;
    constexpr double FAR_OFF = 5;
    const QSize size(480, 360);
    const QRect all(QPoint(0, 0), size);
    const CardScaler::Output output = { size, bt601, false };

    AVBufferRef *device = nullptr;
    if (!Core::Media::hardwareDeviceCanBeAskedFor(cardDevice()) || av_hwdevice_ctx_create(&device, cardDevice(), nullptr, nullptr, 0) < 0)
        QSKIP("the graphics card has no device for pictures that stay on it");
    AVFrame *large = quartersOnCard(device, QSize(1280, 960));
    AVFrame *another = quartersOnCard(device, QSize(1280, 960));
    AVFrame *lesser = quartersOnCard(device, QSize(640, 480));
    AVFrame *scaled = av_frame_alloc();
    AVFrame *inMemory = av_frame_alloc();
    const auto release = qScopeGuard([&] {
        av_frame_free(&inMemory);
        av_frame_free(&scaled);
        av_frame_free(&lesser);
        av_frame_free(&another);
        av_frame_free(&large);
        av_buffer_unref(&device);
    });
    QVERIFY(large && another && lesser);

    CardScaler scaler;
    if (!scaler.scale(large, output, scaled)) {
        QVERIFY(!CardScaler::usable());
        QSKIP("this machine's graphics card does not scale pictures");
    }
    QVERIFY(CardScaler::usable());

    const auto shows = [&](int perMilleX, int perMilleY, int quarter, const char *where) {
        const int x = size.width() * perMilleX / 1000;
        const int y = size.height() * perMilleY / 1000;
        const Yuv seen = colourAt(inMemory, x, y);
        const Yuv expected = quarter < 0 ? Yuv{ 16, 128, 128 } : inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], !bt601);
        const double off = distance(seen, expected);
        const double offTheOther = quarter < 0 ? 0 : distance(seen, inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], bt601));
        qInfo().noquote() << QStringLiteral("%1: %2 %3 %4, expected %5 %6 %7, off by %8 (off the other coefficients by %9)").arg(QLatin1String(where)).arg(seen.y).arg(seen.u).arg(seen.v).arg(expected.y, 0, 'f', 1).arg(expected.u, 0, 'f', 1).arg(expected.v, 0, 'f', 1).arg(off, 0, 'f', 1).arg(offTheOther, 0, 'f', 1);
        return off < CLOSE && (quarter < 0 || quarter == 3 || offTheOther > FAR_OFF);
    };
    const auto takenOffTheCard = [&] {
        av_frame_unref(inMemory);
        return av_hwframe_transfer_data(inMemory, scaled, 0) >= 0 && inMemory->format == AV_PIX_FMT_NV12 && QSize(inMemory->width, inMemory->height) == size;
    };

    QCOMPARE(scaled->format, static_cast<int>(cardFormat(cardDevice())));
    QCOMPARE(formatOnCard(scaled), AV_PIX_FMT_NV12);
    QCOMPARE(QSize(scaled->width, scaled->height), size);
    QVERIFY(reinterpret_cast<const AVHWFramesContext *>(scaled->hw_frames_ctx->data)->device_ref->data == device->data);
    QCOMPARE(static_cast<int>(scaled->colorspace), static_cast<int>(bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709));
    QCOMPARE(static_cast<int>(scaled->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
    const uint8_t *const kind = scaled->hw_frames_ctx->data;

    QVERIFY(takenOffTheCard());
    QVERIFY(shows(250, 250, 0, "the whole picture, top left"));
    QVERIFY(shows(750, 250, 1, "the whole picture, top right"));
    QVERIFY(shows(250, 750, 2, "the whole picture, bottom left"));
    QVERIFY(shows(750, 750, 3, "the whole picture, bottom right"));

    QVERIFY(scaler.scale(large, QRect(640, 0, 640, 960), output, all, scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(100, 250, 1, "the right half, top left"));
    QVERIFY(shows(900, 250, 1, "the right half, top right"));
    QVERIFY(shows(100, 750, 3, "the right half, bottom left"));
    QVERIFY(shows(900, 750, 3, "the right half, bottom right"));
    QVERIFY(scaler.scale(large, QRect(0, 480, 640, 480), output, all, scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(100, 100, 2, "the bottom left quarter, top left"));
    QVERIFY(shows(900, 900, 2, "the bottom left quarter, bottom right"));

    large->crop_left = 640;
    QVERIFY(scaler.scale(large, output, scaled));
    large->crop_left = 0;
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(100, 250, 1, "cropped to its right half, top left"));
    QVERIFY(shows(900, 750, 3, "cropped to its right half, bottom right"));
    QVERIFY(scaled->hw_frames_ctx->data == kind);

#ifdef Q_OS_WIN
    QVERIFY(scaler.scale(large, QRect(0, 0, 1280, 960), output, QRect(120, 90, 240, 180), scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(375, 375, 0, "placed in the middle, its top left"));
    QVERIFY(shows(625, 625, 3, "placed in the middle, its bottom right"));
    QVERIFY(shows(100, 100, -1, "placed in the middle, outside it at the top left"));
    QVERIFY(shows(900, 500, -1, "placed in the middle, outside it on the right"));
    QVERIFY(scaled->hw_frames_ctx->data == kind);

    QVERIFY(scaler.scale(lesser, output, scaled));
    QVERIFY(scaled->hw_frames_ctx->data == kind);
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(250, 250, 0, "the smaller source, top left"));
    QVERIFY(shows(750, 750, 3, "the smaller source, bottom right"));
    QVERIFY(scaler.scale(large, output, scaled));
    QVERIFY(scaled->hw_frames_ctx->data == kind);
    const AVFrame *const other = lesser;
#else
    // VAAPI's scaler fills the whole picture or does nothing
    QVERIFY(!scaler.scale(large, QRect(0, 0, 1280, 960), output, QRect(120, 90, 240, 180), scaled));
    QVERIFY(CardScaler::usable());

    QVERIFY(scaler.scale(another, output, scaled));
    QVERIFY(scaled->hw_frames_ctx->data == kind);
    QVERIFY(scaler.scale(lesser, output, scaled));
    QVERIFY(takenOffTheCard());
    QVERIFY(shows(250, 250, 0, "the smaller source, top left"));
    QVERIFY(shows(750, 750, 3, "the smaller source, bottom right"));
    const AVFrame *const other = another;
#endif

    for (int index = 0; index < 8; index++)
        QVERIFY(scaler.scale(index % 2 ? other : large, output, scaled));
}

void TestVideoEncode::picturesScaledOnTheCardAreEncodedThere_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    QTest::addColumn<EncoderCase>("encoderCase");
    for (VideoCodec codec : CODECS) {
        for (const QString &name : Core::Media::videoEncoderNames(codec))
            QTest::newRow(qPrintable(name)) << EncoderCase{ codec, name };
    }
}

void TestVideoEncode::picturesScaledOnTheCardAreEncodedThere()
{
    QFETCH(EncoderCase, encoderCase);
    SKIP_AN_ENCODER_THAT_DOES_NOT_WORK(encoderCase);
    using Core::Video::CardScaler;
    constexpr int PICTURES = 40;
    const Core::Video::CardScaler::Output output = { QSize(WIDTH, HEIGHT), false, false };

    VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
    if (!encoder.takesCardFrames())
        QSKIP("this encoder takes no pictures from the graphics card");
    if (cardDevice(encoderCase.encoderName) != cardDevice())
        QSKIP("this encoder takes pictures of another kind than the card's scaler makes");
    CardPictures pictures(AV_PIX_FMT_BGRA, 2 * WIDTH, 2 * HEIGHT, AVCOL_SPC_RGB);
    if (!pictures.usable())
        QSKIP("the graphics card has no device that takes these pictures");

    CardScaler scaler;
    AVFrame *scaled = av_frame_alloc();
    const auto freeScaled = qScopeGuard([&scaled] { av_frame_free(&scaled); });
    std::vector<EncodedVideoFrame> frames;
    for (int index = 0; index < PICTURES; index++) {
        AVFrame *picture = pictures.next(index);
        QVERIFY(picture);
        if (!scaler.scale(picture, output, scaled)) {
            QVERIFY(!CardScaler::usable());
            QSKIP("this machine's graphics card does not scale pictures");
        }
        QCOMPARE(formatOnCard(scaled), AV_PIX_FMT_NV12);
        QVERIFY2(encoder.encode(scaled, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded, qPrintable(QStringLiteral("picture %1 was not taken from the card").arg(index)));
        QCOMPARE(frames.size(), size_t(index + 1));
    }
    qInfo() << "encoder:" << encoder.stats().encoderName << encoder.stats().averageEncodeMs << "ms a picture";
    QVERIFY(encoder.stats().encoderName.endsWith(pictures.device));
    QCOMPARE(encoder.stats().opened, quint64(1));
    for (int index = 0; index < PICTURES; index++) {
        QCOMPARE(frames[static_cast<size_t>(index)].keyframe, index == 0);
        QVERIFY(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
    }

    const Decoded decoded = decodeAll(encoderCase.codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, PICTURES);
    QVERIFY(isThePictureSent(encoderCase.codec, decoded, output.size));
    qInfo() << "the patch decodes to" << decoded.middle.y << decoded.middle.u << decoded.middle.v << ", off BT.709 by" << distance(decoded.middle, patchInYuv(true)) << "and off BT.601 by" << distance(decoded.middle, patchInYuv(false)) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY(distance(decoded.middle, patchInYuv(true)) < 6);
    QVERIFY(distance(decoded.middle, patchInYuv(false)) > 12);
    QCOMPARE(decoded.colorspace, static_cast<int>(AVCOL_SPC_BT709));
}

void TestVideoEncode::screensAreListedWithThumbnails()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Core::Video::screenCaptureAvailable()) {
        QVERIFY(Core::Video::captureSources().isEmpty());
        QSKIP("this build or this machine cannot capture a screen");
    }
#ifndef Q_OS_WIN
    {
        using namespace Core::Video;
        if (captureMethodUsable(CaptureMethod::Portal)) {
            const QList<CaptureSource> offered = captureSources(CaptureMethod::Portal);
            QVERIFY(offered.size() == 1 || offered.size() == 2);
            QCOMPARE(offered.first().kind, CaptureSource::Kind::Screen);
            QVERIFY(isChosenByTheDesktop(offered.first()));
            QVERIFY(!isHandedOverByTheDesktop(offered.first()));
            QVERIFY(!offered.first().name.isEmpty());
            QVERIFY(offered.first().geometry.isEmpty());
            QVERIFY(captureThumbnail(offered.first(), QSize(320, 180)).isNull());
            QCOMPARE(encodeSizeOf(offered.first(), 720), QSize());
        }
        if (!captureMethodUsable(CaptureMethod::X11))
            QSKIP("only X11 lists the screens and has thumbnails of them, and it does not show this desktop");
        setCaptureMethod(CaptureMethod::X11);
    }
#endif
    const QList<Core::Video::CaptureSource> sources = Core::Video::captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");

    const QSize bound(320, 180);
    for (int i = 0; i < sources.size(); i++) {
        QVERIFY(sources[i].id.size() > QStringLiteral("screen:").size() && sources[i].id.startsWith(QStringLiteral("screen:")));
        for (int j = 0; j < i; j++)
            QVERIFY(sources[j].id != sources[i].id);
        QVERIFY(!sources[i].name.isEmpty());
        QVERIFY(sources[i].geometry.width() >= 320 && sources[i].geometry.height() >= 200);

        QElapsedTimer timer;
        timer.start();
        const QImage thumbnail = Core::Video::captureThumbnail(sources[i], bound);
        qInfo() << sources[i].id << sources[i].name << sources[i].geometry << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
        QVERIFY(!thumbnail.isNull());
        QCOMPARE(thumbnail.size(), sources[i].geometry.size().scaled(bound, Qt::KeepAspectRatio));
        QCOMPARE(thumbnail.format(), QImage::Format_RGB32);
        QCOMPARE(*reinterpret_cast<const quint32 *>(thumbnail.constScanLine(0)) >> 24, 0xFFu);
    }
    QVERIFY(Core::Video::captureThumbnail({}, bound).isNull());
}

void TestVideoEncode::thePipelineSharesTheScreen_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using Core::Video::CaptureMethod;
    QTest::addColumn<EncoderCase>("encoderCase");
    QTest::addColumn<CaptureMethod>("method");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : encodable)
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p"))) << EncoderCase{ codec, {}, 720 } << CaptureMethod::Auto;
    if (!encodable.isEmpty()) {
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at the screen's own size"))) << EncoderCase{ encodable.first(), {}, 0 } << CaptureMethod::Auto;
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at 720p, by graphics capture"))) << EncoderCase{ encodable.first(), {}, 720 } << CaptureMethod::GraphicsCapture;
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at the screen's own size, by graphics capture"))) << EncoderCase{ encodable.first(), {}, 0 } << CaptureMethod::GraphicsCapture;
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at 720p, a second screen by duplication"))) << EncoderCase{ encodable.first(), {}, 720 } << CaptureMethod::DesktopDuplication;
        QTest::newRow(qPrintable(videoCodecName(encodable.first()) + QStringLiteral(" at 720p, by screen copy"))) << EncoderCase{ encodable.first(), {}, 720 } << CaptureMethod::ScreenCopy;
    }
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    for (VideoCodec codec : encodable) {
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p, handed over by PipeWire"))) << EncoderCase{ codec, {}, 720 } << CaptureMethod::Portal;
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at the screen's own size, handed over by PipeWire"))) << EncoderCase{ codec, {}, 0 } << CaptureMethod::Portal;
    }
#endif
#ifdef ACHERON_TESTS_X11
    for (VideoCodec codec : encodable) {
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p, a second screen on X11"))) << EncoderCase{ codec, {}, 720 } << CaptureMethod::X11;
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at the screen's own size, a second screen on X11"))) << EncoderCase{ codec, {}, 0 } << CaptureMethod::X11;
    }
#endif
}

void TestVideoEncode::thePipelineSharesTheScreen()
{
    if (Core::Media::encodableVideoCodecs().isEmpty())
        QSKIP("this machine has no encoder");
    QFETCH(EncoderCase, encoderCase);
    QFETCH(Core::Video::CaptureMethod, method);
    using namespace Core::Video;
    if (!screenCaptureAvailable())
        QSKIP("this build or this machine cannot capture a screen");
    if (!captureMethodUsable(method))
        QSKIP("this build or this system does not have the capture method");
    const QList<CaptureSource> sources = captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");
    if (!aScreenIsOnAGraphicsCard())
        QSKIP("no screen of this machine is on a graphics card");
    setCaptureMethod(method);
    // the screen holds still, and the pictures' pacing is measured
    QSettings().setValue(QStringLiteral("video/skip_unchanged_pictures"), false);
    const auto skipAgain = qScopeGuard([] { QSettings().remove(QStringLiteral("video/skip_unchanged_pictures")); });
    CaptureSource source = sources.first();
#ifdef Q_OS_WIN
    std::unique_ptr<TestWindow> stirring;
    {
        SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
        const auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
        QVERIFY(shared != sources.end());
        source = *shared;
        stirring = std::make_unique<TestWindow>(TestWindow::Look{ QSize(440, 330), quiet->whole.center() - QPoint(220, 190) });
        QVERIFY(stirring->isOpen());
        QVERIFY(stirring->insideOnScreen().adjusted(40, 40, -40, -40).contains(quiet->whole.center()));
    }
#endif
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    std::unique_ptr<StandInDesktop> desktop;
    if (method == CaptureMethod::Portal) {
        desktop = std::make_unique<StandInDesktop>(STAND_IN_SCREEN);
        if (!desktop->isOpen())
            QSKIP(NO_PIPEWIRE);
        source = desktop->source();
    } else if (isChosenByTheDesktop(source)) {
        QSKIP("the screen to share is chosen in the desktop's own dialog, which no test opens");
    }
#endif
#ifdef ACHERON_TESTS_X11
    std::unique_ptr<TestWindow> stirring;
    if (method == CaptureMethod::X11) {
        SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
        const auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
        QVERIFY(shared != sources.end());
        source = *shared;
        stirring = std::make_unique<TestWindow>(windowLook(QSize(440, 330), quiet->whole.center() - QPoint(220, 190)));
        QVERIFY(stirring->isOpen());
        QTRY_VERIFY2(stirring->insideOnScreen().adjusted(40, 40, -40, -40).contains(quiet->whole.center()), qPrintable(QStringLiteral("the window's inside is at %1,%2 and %3x%4").arg(stirring->insideOnScreen().x()).arg(stirring->insideOnScreen().y()).arg(stirring->insideOnScreen().width()).arg(stirring->insideOnScreen().height())));
        QTRY_VERIFY(isColour(onTheScreen(*quiet, quiet->whole.center()), SHARED_COLOUR));
    }
#endif

    const QSize expectedSize = encodeSizeFor(source.geometry.size(), limitOf(encoderCase.detail));
    const QList<VideoCodec> shared = codecsSharedAt(expectedSize);
    if (!shared.contains(encoderCase.codec) && method == CaptureMethod::Auto)
        QSKIP("a screen of this size is not shared in this codec, whose encoder would add black to it");
    const VideoCodec codec = shared.contains(encoderCase.codec) ? encoderCase.codec : shared.first();

    ScreenPipeline pipeline;
    QObject here;
    std::vector<EncodedFramePointer> frames;
    QList<QSize> starts;
    QList<QImage> previews;
    QList<PipelineStats> stats;
    QList<bool> stalls;
    QStringList failures;
    connect(&pipeline, &ScreenPipeline::started, &here, [&](const QSize &size, int framerate) {
        QCOMPARE(framerate, FRAMERATE);
        starts.append(size);
    });
    connect(&pipeline, &ScreenPipeline::frameEncoded, &here, [&](const EncodedFramePointer &frame) { frames.push_back(frame); });
    connect(&pipeline, &ScreenPipeline::previewReady, &here, [&](const QImage &preview) { previews.append(preview); });
    connect(&pipeline, &ScreenPipeline::statsUpdated, &here, [&](const PipelineStats &update) { stats.append(update); });
    connect(&pipeline, &ScreenPipeline::captureStalled, &here, [&](bool stalled) { stalls.append(stalled); });
    connect(&pipeline, &ScreenPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });

    ScreenSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = limitOf(encoderCase.detail);
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;

    QElapsedTimer sinceStart;
    sinceStart.start();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 20 || !failures.isEmpty(), "fewer than 20 frames in 10 s", 10000);
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char(';'))));
    qInfo() << "20 frames after" << sinceStart.elapsed() << "ms";

    QCOMPARE(starts.size(), 1);
    QCOMPARE(starts.first(), expectedSize);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(!frames[1]->keyframe);

    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(frames[i]->rtpTimestamp - frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(frames[19]->rtpTimestamp - frames[0]->rtpTimestamp) / 19.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8 && ticksPerFrame < TICKS_PER_FRAME * 1.25, qPrintable(QString::number(ticksPerFrame)));

    const size_t atRequest = frames.size();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atRequest + 6, 1000);
    const auto forced = std::find_if(frames.begin() + static_cast<std::ptrdiff_t>(atRequest), frames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; });
    QVERIFY(forced != frames.end());
    QVERIFY(forced - frames.begin() < static_cast<std::ptrdiff_t>(atRequest) + 4);

    QTRY_VERIFY_WITH_TIMEOUT(!previews.isEmpty(), 2000);
    pipeline.setEncodingSuspended(true);
    QTest::qWait(300);
    const size_t whileSuspended = frames.size();
    const int previewsBefore = previews.size();
    QTest::qWait(1200);
    QCOMPARE(frames.size(), whileSuspended);
    QVERIFY(previews.size() >= previewsBefore + 2);

    pipeline.setEncodingSuspended(false);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= whileSuspended + 10, 1500);
    QVERIFY(frames[whileSuspended]->keyframe);
    QVERIFY(!frames[whileSuspended + 1]->keyframe);

    pipeline.setBitrate(1'000'000);
    const size_t atNewRate = frames.size();
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= atNewRate + 15, 1500);

    for (const QImage &preview : previews) {
        QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 512 && preview.height() <= 288);
        QCOMPARE(preview.size(), expectedSize.scaled(QSize(512, 288), Qt::KeepAspectRatio));
    }

    const int statsBefore = static_cast<int>(stats.size());
    QTRY_VERIFY_WITH_TIMEOUT(stats.size() >= statsBefore + 2, 3500);
    const PipelineStats lastStats = stats.last();
    qInfo().noquote() << QStringLiteral("pipeline: %1, hardware %2, %3x%4, captured %5 fps, %6 ms in the encoder a picture, %7 frames, %8 dropped").arg(lastStats.encoderName).arg(lastStats.hardware).arg(lastStats.encodedSize.width()).arg(lastStats.encodedSize.height()).arg(lastStats.capturedFps, 0, 'f', 1).arg(lastStats.averageEncodeMs, 0, 'f', 2).arg(lastStats.framesEncoded).arg(lastStats.framesDropped);
    QCOMPARE(lastStats.encodedSize, expectedSize);
#ifdef Q_OS_WIN
    constexpr double FEWEST = 0.6;
#else
    constexpr double FEWEST = 0.8;
#endif
    QVERIFY2(lastStats.capturedFps > FRAMERATE * FEWEST && lastStats.capturedFps < FRAMERATE * 1.2, qPrintable(QString::number(lastStats.capturedFps)));
#ifdef Q_OS_WIN
    QVERIFY2(lastStats.encoderName.endsWith(QStringLiteral(", from ") + captureNameOf(method)), qPrintable(lastStats.encoderName));
    const bool copiedIntoMemory = method == CaptureMethod::ScreenCopy;
    QCOMPARE(lastStats.encoderName.contains(onDevice(cardDevice())), !copiedIntoMemory && reachesTheEncoderOnTheCard(codec, expectedSize != source.geometry.size(), method == CaptureMethod::GraphicsCapture));
#endif
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    if (desktop) {
        QVERIFY2(lastStats.encoderName.endsWith(QStringLiteral(", from pipewire memfd BGRx")), qPrintable(lastStats.encoderName));
        QVERIFY2(!lastStats.encoderName.contains(QStringLiteral(" (")), "pictures handed over in memory reached the encoder some other way");
    }
#endif
#ifdef ACHERON_TESTS_X11
    if (!isHandedOverByTheDesktop(source)) {
        QVERIFY2(lastStats.encoderName.endsWith(QStringLiteral(", from x11grab")), qPrintable(lastStats.encoderName));
        QVERIFY2(!lastStats.encoderName.contains(QStringLiteral(" (")), "pictures grabbed into memory reached the encoder some other way");
    }
#endif

    pipeline.stop();
    QTest::qWait(300);
    const size_t afterStop = frames.size();
    QTest::qWait(300);
    QCOMPARE(frames.size(), afterStop);
    QVERIFY(failures.isEmpty());
    QVERIFY(stalls.isEmpty());

    for (const EncodedFramePointer &frame : frames)
        QVERIFY(isWellFormed(codec, *frame));
    const Decoded decoded = decodeAll(codec, frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames.size()));
    QVERIFY2(isThePictureSent(codec, decoded, expectedSize), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
#ifdef Q_OS_WIN
    if (stirring) {
        qInfo() << "the middle of the screen decodes off the window's colour by" << offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
        QVERIFY2(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window in the middle of the screen is not its colour in the picture");
    }
#endif
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    if (desktop) {
        qInfo() << "the middle of the picture decodes off the colour handed over by" << offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
        QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the colour handed over is not the one in the middle of the picture");
        QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.top, SHARED_COLOUR) < COLOUR_SURVIVES, "the colour handed over is not the one at the top of the picture");
    }
#endif
#ifdef ACHERON_TESTS_X11
    if (stirring) {
        qInfo() << "the middle of the screen decodes off the window's colour by" << offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
        QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window in the middle of the screen is not its colour in the picture");
    }
#endif

    const uint32_t lastTimestamp = frames.back()->rtpTimestamp;
    frames.clear();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(frames.size() >= 5 || !failures.isEmpty(), "no frames after a restart", 3000);
    QVERIFY(failures.isEmpty());
    QCOMPARE(starts.size(), 2);
    QVERIFY(frames[0]->keyframe);
    QVERIFY(static_cast<int32_t>(frames[0]->rtpTimestamp - lastTimestamp) > 0);
}

void TestVideoEncode::aScaledScreenIsHandedOverAsItsEncoderTakesIt_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using Core::Video::CaptureMethod;
    QTest::addColumn<CaptureMethod>("method");
    QTest::newRow("by duplication") << CaptureMethod::DesktopDuplication;
    QTest::newRow("by graphics capture") << CaptureMethod::GraphicsCapture;
    QTest::newRow("by screen copy") << CaptureMethod::ScreenCopy;
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QTest::newRow("handed over by PipeWire") << CaptureMethod::Portal;
#endif
#ifdef ACHERON_TESTS_X11
    QTest::newRow("grabbed from X11") << CaptureMethod::X11;
#endif
}

#ifdef ACHERON_TESTS_X11
namespace {

void aScaledScreenOfX11IsHandedOverAsItsEncoderTakesIt()
{
    using namespace Core::Video;
    constexpr int SETTLING = 15;
    constexpr int PICTURES = 60;
    constexpr int MAX_HEIGHT = 720;
    if (!captureMethodUsable(CaptureMethod::X11))
        QSKIP(NO_X11);
    const QList<CaptureSource> sources = captureSources(CaptureMethod::X11);
    if (sources.isEmpty())
        QSKIP("no screen is attached");
    const CaptureSource source = sources.first();
    const QSize size = encodeSizeFor(source.geometry.size(), MAX_HEIGHT);
    if (size == source.geometry.size())
        QSKIP("the screen is no larger than 720p, so nothing is scaled");

    struct Way
    {
        const char *name;
        bool keepOnCard;
    };
    const std::array ways = {
        Way{ "grabbed into memory and scaled in software", false },
        Way{ "grabbed into memory by a capture asked to keep its pictures on the card", true },
    };
    for (const Way &way : ways) {
        ScreenCapture::Request request;
        request.sourceId = source.id;
        request.method = CaptureMethod::X11;
        request.maxHeight = MAX_HEIGHT;
        request.framerate = FRAMERATE;
        request.softwareFormat = AV_PIX_FMT_NV12;
        request.keepOnCard = way.keepOnCard;
        ScreenCapture capture;
        capture.configure(request);

        AVFrame *picture = av_frame_alloc();
        const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
        int read = 0;
        int onCard = 0;
        int nv12 = 0;
        double cpuAfterSettling = 0;
        QElapsedTimer timer;
        timer.start();
        qint64 settledAt = 0;
        while (read < SETTLING + PICTURES && timer.elapsed() < 10000) {
            const ScreenCapture::Status status = capture.read(picture);
            QVERIFY2(status != ScreenCapture::Status::Failed && status != ScreenCapture::Status::Closed, qPrintable(capture.failure()));
            if (status != ScreenCapture::Status::Frame)
                continue;
            QCOMPARE(QSize(picture->width, picture->height), size);
            QCOMPARE(static_cast<int>(picture->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
            QCOMPARE(static_cast<int>(picture->colorspace), static_cast<int>(AVCOL_SPC_BT709));
            onCard += picture->hw_frames_ctx ? 1 : 0;
            nv12 += picture->format == AV_PIX_FMT_NV12 ? 1 : 0;
            av_frame_unref(picture);
            if (++read == SETTLING) {
                cpuAfterSettling = processCpuMs();
                settledAt = timer.elapsed();
            }
        }
        QCOMPARE(read, SETTLING + PICTURES);
        const double framesPerSecond = PICTURES * 1000.0 / static_cast<double>(timer.elapsed() - settledAt);
        qInfo().noquote() << QStringLiteral("a %1x%2 screen captured as %3x%4, %5: %6 ms of processor time a picture, %7 pictures a second; by %8").arg(source.geometry.width()).arg(source.geometry.height()).arg(size.width()).arg(size.height()).arg(QLatin1String(way.name)).arg((processCpuMs() - cpuAfterSettling) / PICTURES, 0, 'f', 2).arg(framesPerSecond, 0, 'f', 1).arg(capture.captureName());

        QCOMPARE(onCard, 0);
        QCOMPARE(nv12, read);
        QCOMPARE(capture.captureName(), QStringLiteral("x11grab"));
        QVERIFY(framesPerSecond > FRAMERATE * 0.9 && framesPerSecond < FRAMERATE * 1.1);
    }
}

} // namespace
#endif

void TestVideoEncode::aScaledScreenIsHandedOverAsItsEncoderTakesIt()
{
#ifdef ACHERON_TESTS_X11
    if (QByteArray(QTest::currentDataTag()) == "grabbed from X11") {
        aScaledScreenOfX11IsHandedOverAsItsEncoderTakesIt();
        return;
    }
#endif
#ifdef Q_OS_WIN
    QFETCH(Core::Video::CaptureMethod, method);
    using namespace Core::Video;
    constexpr int PICTURES = 15;
    constexpr int MAX_HEIGHT = 720;
    if (!screenCaptureAvailable() || !captureMethodUsable(method))
        QSKIP("this build or this Windows cannot capture a screen this way");
    const QList<CaptureSource> sources = captureSources();
    if (sources.isEmpty())
        QSKIP("no screen is attached");
    if (!aScreenIsOnAGraphicsCard())
        QSKIP("no screen of this machine is on a graphics card");

    CaptureSource source = sources.first();
    std::unique_ptr<TestWindow> stirring;
    {
        SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
        const auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
        QVERIFY(shared != sources.end());
        source = *shared;
        stirring = std::make_unique<TestWindow>(TestWindow::Look{ QSize(96, 96), quiet->cornerFor(QSize(160, 180)) });
    }
    const QSize size = encodeSizeFor(source.geometry.size(), MAX_HEIGHT);
    if (size == source.geometry.size())
        QSKIP("the screen is no larger than 720p, so nothing is scaled");
    // the screen holds still, and every picture is to be looked at
    QSettings().setValue(QStringLiteral("video/skip_unchanged_pictures"), false);
    const auto skipAgain = qScopeGuard([] { QSettings().remove(QStringLiteral("video/skip_unchanged_pictures")); });

    struct Way
    {
        const char *name;
        bool keepOnCard;
        bool rgbOnCard;
    };
    const std::array ways = {
        Way{ "read back and scaled in software", false, false },
        Way{ "kept on the card for an encoder that takes NV12 and the screen's own BGRA", true, true },
        Way{ "kept on the card for an encoder that takes only NV12", true, false },
    };
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const bool anEncoderTakesCardFrames = std::any_of(encodable.begin(), encodable.end(), [](VideoCodec codec) { return VideoEncoder(settingsFor({ codec, {} }, BITRATE)).takesCardFrames(); });
    for (const Way &way : ways) {
        if (way.keepOnCard && !anEncoderTakesCardFrames)
            continue;
        ScreenCapture::Request request;
        request.sourceId = source.id;
        request.method = method;
        request.maxHeight = MAX_HEIGHT;
        request.framerate = FRAMERATE;
        request.softwareFormat = AV_PIX_FMT_NV12;
        request.keepOnCard = way.keepOnCard;
        request.rgbOnCard = way.rgbOnCard;
        ScreenCapture capture;
        capture.configure(request);

        AVFrame *picture = av_frame_alloc();
        const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
        int read = 0;
        int onCard = 0;
        int nv12 = 0;
        QElapsedTimer timer;
        timer.start();
        while (read < PICTURES && timer.elapsed() < 10000) {
            const ScreenCapture::Status status = capture.read(picture);
            QVERIFY2(status != ScreenCapture::Status::Failed && status != ScreenCapture::Status::Closed, qPrintable(capture.failure()));
            if (status != ScreenCapture::Status::Frame)
                continue;
            QCOMPARE(QSize(picture->width, picture->height), size);
            QCOMPARE(static_cast<int>(picture->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
            QCOMPARE(static_cast<int>(picture->colorspace), static_cast<int>(AVCOL_SPC_BT709));
            onCard += picture->hw_frames_ctx ? 1 : 0;
            nv12 += (picture->hw_frames_ctx ? formatOnCard(picture) : static_cast<AVPixelFormat>(picture->format)) == AV_PIX_FMT_NV12 ? 1 : 0;
            av_frame_unref(picture);
            read++;
        }
        QCOMPARE(read, PICTURES);

        const bool scaledByTheSource = method == CaptureMethod::GraphicsCapture && way.rgbOnCard;
        const bool copiedIntoMemory = method == CaptureMethod::ScreenCopy;
        QVERIFY2((onCard == (way.keepOnCard && !copiedIntoMemory && (scaledByTheSource || CardScaler::usable()) ? read : 0)) && nv12 == (scaledByTheSource ? 0 : read), qPrintable(QStringLiteral("%1: %2 of %3 pictures on the card, %4 of them NV12").arg(QLatin1String(way.name)).arg(onCard).arg(read).arg(nv12)));
    }
#elif defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QFETCH(Core::Video::CaptureMethod, method);
    using namespace Core::Video;
    constexpr int SETTLING = 15;
    constexpr int PICTURES = 60;
    constexpr int MAX_HEIGHT = 720;
    if (method != CaptureMethod::Portal)
        QSKIP("this is one of Windows' ways of capturing a screen");
    const QSize size = encodeSizeFor(STAND_IN_SCREEN, MAX_HEIGHT);

    struct Way
    {
        const char *name;
        bool buffersOnTheCard;
        bool keepOnCard;
    };
    const std::array ways = {
        Way{ "handed over in memory and scaled in software", false, false },
        Way{ "handed over in memory to a capture asked to keep its pictures on the card", false, true },
        Way{ "handed over on the card and scaled there", true, true },
    };
    for (const Way &way : ways) {
        if (way.buffersOnTheCard && !PipeWireTestSource::cardMemoryCanBeMade()) {
            qInfo() << NO_CARD_BUFFERS;
            continue;
        }
        StandInDesktop desktop(STAND_IN_SCREEN, way.buffersOnTheCard);
        if (!desktop.isOpen())
            QSKIP(NO_PIPEWIRE);
        ScreenCapture::Request request;
        request.sourceId = desktop.source().id;
        request.method = method;
        request.maxHeight = MAX_HEIGHT;
        request.framerate = FRAMERATE;
        request.softwareFormat = AV_PIX_FMT_NV12;
        request.keepOnCard = way.keepOnCard;
        ScreenCapture capture;
        capture.configure(request);

        AVFrame *picture = av_frame_alloc();
        const auto freePicture = qScopeGuard([&picture] { av_frame_free(&picture); });
        const Heard log;
        int read = 0;
        int onCard = 0;
        int nv12 = 0;
        double cpuAfterSettling = 0;
        QElapsedTimer timer;
        timer.start();
        while (read < SETTLING + PICTURES && timer.elapsed() < 10000) {
            const ScreenCapture::Status status = capture.read(picture);
            QVERIFY2(status != ScreenCapture::Status::Failed && status != ScreenCapture::Status::Closed, qPrintable(capture.failure()));
            if (status != ScreenCapture::Status::Frame)
                continue;
            QCOMPARE(QSize(picture->width, picture->height), size);
            QCOMPARE(static_cast<int>(picture->color_range), static_cast<int>(AVCOL_RANGE_MPEG));
            QCOMPARE(static_cast<int>(picture->colorspace), static_cast<int>(AVCOL_SPC_BT709));
            onCard += picture->hw_frames_ctx ? 1 : 0;
            nv12 += (picture->hw_frames_ctx ? formatOnCard(picture) : static_cast<AVPixelFormat>(picture->format)) == AV_PIX_FMT_NV12 ? 1 : 0;
            av_frame_unref(picture);
            if (++read == SETTLING)
                cpuAfterSettling = processCpuMs();
        }
        QCOMPARE(read, SETTLING + PICTURES);
        const bool refused = Heard::heard(QStringLiteral("would not take or scale"));
        qInfo().noquote() << QStringLiteral("%1x%2 pictures captured as %3x%4, %5: %6 ms of processor time a picture, the test's own sending included; %7 of %8 pictures on the card, %9 of them NV12; by %10%11").arg(STAND_IN_SCREEN.width()).arg(STAND_IN_SCREEN.height()).arg(size.width()).arg(size.height()).arg(QLatin1String(way.name)).arg((processCpuMs() - cpuAfterSettling) / PICTURES, 0, 'f', 2).arg(onCard).arg(read).arg(nv12).arg(capture.captureName(), refused ? QStringLiteral(", after the card would not take the test's buffers") : QString());

        QCOMPARE(onCard, way.buffersOnTheCard && way.keepOnCard && CardScaler::usable() && !refused ? read : 0);
        QCOMPARE(nv12, read);
        QCOMPARE(capture.captureName(), onCard ? QStringLiteral("pipewire dmabuf BGRx") : QStringLiteral("pipewire memfd BGRx"));
    }
#else
    QSKIP("this build captures no screen");
#endif
}

void TestVideoEncode::aScreenThatIsNotThereFails()
{
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");

    ScreenPipeline pipeline;
    QObject here;
    QStringList failures;
    int framesSeen = 0;
    int starts = 0;
    connect(&pipeline, &ScreenPipeline::failed, &here, [&](const QString &reason) { failures.append(reason); });
    connect(&pipeline, &ScreenPipeline::frameEncoded, &here, [&](const EncodedFramePointer &) { framesSeen++; });
    connect(&pipeline, &ScreenPipeline::started, &here, [&](const QSize &, int) { starts++; });

    ScreenSettings settings;
    settings.sourceId = QStringLiteral("screen:99");
    settings.codec = encodable.first();
    settings.bitrateBps = 1'000'000;
    pipeline.start(settings);

    QTest::qWait(1000);
    if (screenCaptureAvailable())
        QVERIFY(failures.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, ScreenCapture::GIVE_UP_AFTER_MS + 2000);
    qInfo() << "failed with:" << failures.first();
    QVERIFY(!failures.first().isEmpty());
    QCOMPARE(framesSeen, 0);
    QCOMPARE(starts, 0);
}

void TestVideoEncode::aPipeWireStreamThatIsNotThereFails()
{
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!captureMethodUsable(CaptureMethod::Portal))
        QSKIP("there is no desktop portal with a screen cast here");
    const CaptureSource neverThere = PortalCapture::adopt(0x7ffffff0, STAND_IN_WINDOW);
    const auto release = qScopeGuard([&neverThere] { PortalCapture::release(neverThere.id); });

    for (const QString &sourceId : { neverThere.id, QStringLiteral("pipewire:987654") }) {
        ScreenPipeline pipeline;
        PipelineRecord record(pipeline);
        ScreenSettings settings;
        settings.sourceId = sourceId;
        settings.codec = encodable.first();
        settings.bitrateBps = 1'000'000;
        pipeline.start(settings);

        QTest::qWait(1000);
        QVERIFY(record.failures.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(record.failures.size(), 1, ScreenCapture::GIVE_UP_AFTER_MS + 2000);
        qInfo() << sourceId << "failed with:" << record.failures.first();
        QVERIFY(!record.failures.first().isEmpty());
        QVERIFY(record.frames.empty());
        QVERIFY(record.starts.isEmpty());
        QCOMPARE(record.closes, 0);
    }
#else
    QSKIP("this build takes nothing from PipeWire");
#endif
}

void TestVideoEncode::picturesTheDesktopLeavesOnTheCardAreSharedFromThere_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QTest::addColumn<EncoderCase>("encoderCase");
    QTest::addColumn<QSize>("pictures");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : encodable) {
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at 720p"))) << EncoderCase{ codec, {}, 720 } << STAND_IN_SCREEN;
        QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(" at the pictures' own size"))) << EncoderCase{ codec, {}, 0 } << STAND_IN_SCREEN;
    }
    for (VideoCodec codec : { VideoCodec::H264, VideoCodec::H265, VideoCodec::AV1 }) {
        if (encodable.contains(codec) && VideoEncoder(settingsFor({ codec, {} }, BITRATE)).takesCardFrames()) {
            QTest::newRow(qPrintable(videoCodecName(codec) + QStringLiteral(", pictures too small to be sent as they are"))) << EncoderCase{ codec, {}, 0 } << TINY_WINDOW;
            break;
        }
    }
#else
    QSKIP("this build takes nothing from PipeWire");
#endif
}

void TestVideoEncode::picturesTheDesktopLeavesOnTheCardAreSharedFromThere()
{
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QFETCH(EncoderCase, encoderCase);
    QFETCH(QSize, pictures);
    using namespace Core::Video;
    if (!PipeWireTestSource::cardMemoryCanBeMade())
        QSKIP(NO_CARD_BUFFERS);
    const QSize expectedSize = encodeSizeFor(pictures.expandedTo(SMALLEST_PICTURE), limitOf(encoderCase.detail));
    const VideoCodec codec = encoderCase.codec;
    if (!codecsSharedAt(expectedSize).contains(codec))
        QSKIP("pictures of this size are not shared in this codec, whose encoder would add black to them");
    StandInDesktop desktop(pictures, true);
    if (!desktop.isOpen())
        QSKIP(NO_PIPEWIRE);

    const Heard log;
    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = desktop.source().id;
    settings.maxHeight = limitOf(encoderCase.detail);
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty(), "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), expectedSize);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(!record.frames[1]->keyframe);

    const double cpuBefore = processCpuMs();
    const size_t framesBefore = record.frames.size();
    QTest::qWait(1500);
    const double cpuPerFrame = (processCpuMs() - cpuBefore) / static_cast<double>(std::max<size_t>(1, record.frames.size() - framesBefore));
    QTRY_VERIFY_WITH_TIMEOUT(record.stats.size() >= 2, 2500);
    const PipelineStats last = record.stats.last();
    pipeline.stop();
    QTest::qWait(300);

    const bool asked = VideoEncoder(settingsFor({ codec, {} }, BITRATE)).takesCardFrames() && CardScaler::usable();
    const bool refused = Heard::heard(QStringLiteral("would not take or scale"));
    if (asked && pictures != expectedSize && encoderCase.detail == 0)
        QVERIFY2(refused, "pictures that have to be put on black were said to have stayed on the card");
    const QString device = onDevice(cardDevice(Core::Media::videoEncoderFor(codec)));
    const char *whatTheCardDid = asked ? "asked and kept them" : "not asked to keep the pictures";
    if (asked && refused)
        whatTheCardDid = "asked, and the share went on in memory";
    qInfo().noquote() << QStringLiteral("%1, %2 fps, %3 ms of processor time a picture, the test's own sending included; the card was %4").arg(last.encoderName).arg(last.capturedFps, 0, 'f', 1).arg(cpuPerFrame, 0, 'f', 2).arg(QLatin1String(whatTheCardDid));
    QVERIFY(!refused || asked);
    QCOMPARE(last.encoderName.endsWith(QStringLiteral(", from pipewire dmabuf BGRx")), asked && !refused);
    QCOMPARE(last.encoderName.endsWith(QStringLiteral(", from pipewire memfd BGRx")), !asked || refused);
    QCOMPARE(last.encoderName.contains(device), asked && !refused);
    QCOMPARE(last.encodedSize, expectedSize);
    QVERIFY(last.capturedFps > FRAMERATE * 0.8 && last.capturedFps < FRAMERATE * 1.2);
    QVERIFY(record.stalls.isEmpty());
    QCOMPARE(record.closes, 0);

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QVERIFY2(isThePictureSent(codec, decoded, expectedSize), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
    qInfo() << "the middle of the picture decodes off the colour handed over by" << offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the colour handed over is not the one in the middle of the picture");
    QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.top, SHARED_COLOUR) < COLOUR_SURVIVES, "the colour handed over is not the one at the top of the picture");
#endif
}

// the machine's own screen: counted and measured, never looked at
void TestVideoEncode::theRealScreenIsSharedOnTheCardAndInMemory()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (!encodable.contains(VideoCodec::H264))
        QSKIP("this machine has no H264 encoder");
    if (!captureMethodUsable(CaptureMethod::Portal))
        QSKIP("there is no desktop portal with a screen cast here");
    const ApprovedScreen approved = ApprovedScreen::restore();
    if (approved.state == ApprovedScreen::State::NeverApproved)
        QSKIP(ApprovedScreen::NEVER_APPROVED);
    QVERIFY2(approved.state == ApprovedScreen::State::Restored, qPrintable(approved.problem));
    const CaptureSource screen = approved.source;
    const auto release = qScopeGuard([&screen] { PortalCapture::release(screen.id); });
    QVERIFY(isHandedOverByTheDesktop(screen));
    QVERIFY(!screen.geometry.isEmpty());
    qInfo() << "the desktop handed its screen of" << screen.geometry.size() << "over" << approved.answeredAfterMs << "ms after it was asked, which is sooner than anybody answers a dialog";

    QList<VideoCodec> codecs = { VideoCodec::H264 };
    if (encodable.contains(VideoCodec::VP8))
        codecs.append(VideoCodec::VP8);
    for (VideoCodec codec : codecs) {
        for (const int maxHeight : { 720, 0 }) {
            const QSize expectedSize = encodeSizeFor(screen.geometry.size(), limitOf(maxHeight));
            QCOMPARE(encodeSizeOf(screen, limitOf(maxHeight)), expectedSize);
            const Heard log;
            ScreenPipeline pipeline;
            PipelineRecord record(pipeline);
            ScreenSettings settings;
            settings.sourceId = screen.id;
            settings.maxHeight = limitOf(maxHeight);
            settings.framerate = FRAMERATE;
            settings.codec = codec;
            settings.bitrateBps = 2'500'000;
            settings.maxBitrateBps = 4'000'000;
            pipeline.start(settings);

            QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 3 || !record.failures.isEmpty() || record.closes > 0, "fewer than 3 frames in 5 s", 5000);
            QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
            QCOMPARE(record.closes, 0);
            QCOMPARE(record.starts.size(), 1);
            QCOMPARE(record.starts.first(), expectedSize);
            QVERIFY(record.frames[0]->keyframe);

            const QStringList others = othersOnTheStreamsCapturedHere();
            const double cpuBefore = processCpuMs();
            const size_t framesBefore = record.frames.size();
            QTest::qWait(2500);
            const size_t framesInTheMeantime = record.frames.size() - framesBefore;
            const double cpuPerFrame = (processCpuMs() - cpuBefore) / static_cast<double>(std::max<size_t>(1, framesInTheMeantime));
            QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
            const PipelineStats last = record.stats.last();
            pipeline.stop();
            QTest::qWait(300);

            const bool asked = VideoEncoder(settingsFor({ codec, {} }, BITRATE)).takesCardFrames() && CardScaler::usable();
            const bool refused = Heard::heard(QStringLiteral("would not take or scale"));
            qInfo().noquote() << QStringLiteral("%1 as %2x%3: %4; %5 frames in 2.5 s, %6 ms of processor time a picture, %7 ms in the encoder a picture%8").arg(videoCodecName(codec)).arg(expectedSize.width()).arg(expectedSize.height()).arg(last.encoderName).arg(framesInTheMeantime).arg(cpuPerFrame, 0, 'f', 2).arg(last.averageEncodeMs, 0, 'f', 2).arg(refused ? QStringLiteral("; the card would not take or scale the desktop's pictures") : QString());
            QVERIFY(framesInTheMeantime >= 3 && framesInTheMeantime <= FRAMERATE * 2.5 * 1.25);
            QCOMPARE(last.encodedSize, expectedSize);
            QVERIFY(record.stalls.isEmpty());
            if (others.isEmpty()) {
                QVERIFY2(!refused, "the card would not take or scale what the desktop left on it");
                QVERIFY2(last.encoderName.contains(asked ? QStringLiteral(", from pipewire dmabuf ") : QStringLiteral(", from pipewire memfd ")), qPrintable(last.encoderName));
                QCOMPARE(last.encoderName.contains(onDevice(cardDevice(Core::Media::videoEncoderFor(codec)))), asked);
            } else {
                qWarning().noquote() << "the desktop's stream is drawn from by" << others.join(QLatin1String(", ")) << "as well, whose choice its buffers are; what kind they are is not held to anything here";
            }

            for (const EncodedFramePointer &frame : record.frames)
                QVERIFY(isWellFormed(codec, *frame));
            int blackPictures = 0;
            const Decoded decoded = decodeAll(codec, record.frames, [&blackPictures](const AVFrame *picture, int) {
                bool anythingShows = false;
                for (int y = 0; y < picture->height && !anythingShows; y += 16) {
                    for (int x = 0; x < picture->width && !anythingShows; x += 16)
                        anythingShows = picture->data[0][y * picture->linesize[0] + x] > 24;
                }
                blackPictures += anythingShows ? 0 : 1;
            });
            QCOMPARE(decoded.errors, quint64(0));
            QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
            QVERIFY2(isThePictureSent(codec, decoded, expectedSize), qPrintable(QStringLiteral("%1x%2").arg(decoded.width).arg(decoded.height)));
            QCOMPARE(blackPictures, 0);
        }
    }
#else
    QSKIP("this build takes nothing from PipeWire");
#endif
}

void TestVideoEncode::theCaptureMethodIsRemembered()
{
    using namespace Core::Video;
    const QString key = QStringLiteral("video/capture_method");
    QCOMPARE(captureMethod(), CaptureMethod::Auto);

    setCaptureMethod(CaptureMethod::GraphicsCapture);
    QCOMPARE(captureMethod(), CaptureMethod::GraphicsCapture);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("graphics_capture"));
    setCaptureMethod(CaptureMethod::DesktopDuplication);
    QCOMPARE(captureMethod(), CaptureMethod::DesktopDuplication);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("desktop_duplication"));
    setCaptureMethod(CaptureMethod::ScreenCopy);
    QCOMPARE(captureMethod(), CaptureMethod::ScreenCopy);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("screen_copy"));
    setCaptureMethod(CaptureMethod::Portal);
    QCOMPARE(captureMethod(), CaptureMethod::Portal);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("portal"));
    setCaptureMethod(CaptureMethod::X11);
    QCOMPARE(captureMethod(), CaptureMethod::X11);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("x11"));
    setCaptureMethod(CaptureMethod::Auto);
    QCOMPARE(captureMethod(), CaptureMethod::Auto);
    QCOMPARE(QSettings().value(key).toString(), QStringLiteral("auto"));

    QSettings().setValue(key, QStringLiteral("something else"));
    QCOMPARE(captureMethod(), CaptureMethod::Auto);
    QSettings().remove(key);
    QCOMPARE(captureMethod(), CaptureMethod::Auto);
}

void TestVideoEncode::aWindowAboveCoversTheOneBelow()
{
    using Core::Video::StackedWindow;
    using Core::Video::windowIsCovered;
    const auto at = [](int x, int y, int width, int height) {
        StackedWindow window;
        window.bounds = QRect(x, y, width, height);
        return window;
    };
    const StackedWindow target = at(100, 100, 400, 300);

    QVERIFY(!windowIsCovered({ target }, 0));
    QVERIFY(windowIsCovered({ at(300, 200, 400, 300), target }, 1));
    QVERIFY(windowIsCovered({ at(499, 399, 50, 50), target }, 1));
    QVERIFY(windowIsCovered({ at(200, 200, 10, 10), target }, 1));
    QVERIFY(!windowIsCovered({ at(500, 100, 200, 300), target }, 1));
    QVERIFY(!windowIsCovered({ at(100, 400, 400, 50), target }, 1));
    QVERIFY(!windowIsCovered({ at(900, 900, 200, 200), at(0, 0, 100, 100), target }, 2));

    QVERIFY(!windowIsCovered({ target, at(0, 0, 2000, 2000) }, 0));
    QVERIFY(windowIsCovered({ at(0, 0, 2000, 2000), target, at(0, 0, 2000, 2000) }, 1));

    for (int ignored = 0; ignored < 6; ignored++) {
        StackedWindow above = at(0, 0, 2000, 2000);
        above.visible = ignored != 0;
        above.minimised = ignored == 1;
        above.cloaked = ignored == 2;
        above.overlay = ignored == 3;
        above.tabProxy = ignored == 4;
        if (ignored == 5)
            above.bounds = QRect();
        QVERIFY2(!windowIsCovered({ above, target }, 1), qPrintable(QString::number(ignored)));
    }
    QVERIFY(windowIsCovered({ at(0, 0, 2000, 2000), at(150, 150, 20, 20), target }, 2));

    StackedWindow preview = at(5000, 5000, 10, 10);
    preview.livePreview = true;
    QVERIFY(windowIsCovered({ preview, target }, 1));
    preview.visible = false;
    QVERIFY(!windowIsCovered({ preview, target }, 1));

    StackedWindow gone = target;
    gone.minimised = true;
    QVERIFY(windowIsCovered({ gone }, 0));
    gone = target;
    gone.cloaked = true;
    QVERIFY(windowIsCovered({ gone }, 0));
    gone = target;
    gone.bounds = QRect(100, 100, 0, 0);
    QVERIFY(windowIsCovered({ gone }, 0));
    QVERIFY(windowIsCovered({ target }, 1));
    QVERIFY(windowIsCovered({}, 0));
}

void TestVideoEncode::windowsAreListedWithoutThisProgramsOwn()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef Q_OS_WIN
    using namespace Core::Video;
    std::unique_ptr<TestWindow> own;
    if (const std::optional<QuietScreen> quiet = quietScreen()) {
        own = std::make_unique<TestWindow>(TestWindow::Look{ QSize(320, 240), quiet->cornerFor(QSize(400, 340)) });
        QVERIFY(own->isOpen());
        QTest::qWait(200);
    }

    const QList<CaptureSource> windows = captureWindows();
    if (!windowCaptureAvailable()) {
        QVERIFY(windows.isEmpty());
        QSKIP("this build or this Windows cannot capture a window");
    }

    // window titles stay out of the log
    qInfo() << windows.size() << "windows listed";
    QStringList ids;
    for (const CaptureSource &window : windows) {
        QCOMPARE(window.kind, CaptureSource::Kind::Window);
        QVERIFY(window.id.startsWith(QStringLiteral("window:")));
        QVERIFY(!ids.contains(window.id));
        ids.append(window.id);
        QVERIFY(!window.name.isEmpty());
        QVERIFY(window.processId != 0);
        QVERIFY(window.processId != GetCurrentProcessId());

        bool numbered = false;
        const HWND handle = reinterpret_cast<HWND>(static_cast<quintptr>(window.id.mid(7).toULongLong(&numbered)));
        QVERIFY(numbered);
        if (IsWindow(handle) && !IsIconic(handle) && IsWindowVisible(handle))
            QVERIFY(window.geometry.width() > 0 && window.geometry.height() > 0);
    }
    if (own)
        QVERIFY(!ids.contains(own->sourceId()));
#else
    using namespace Core::Video;
    if (!windowCaptureAvailable()) {
        QVERIFY(captureWindows().isEmpty());
        QSKIP("this build or this machine cannot capture a window");
    }
    if (captureMethodUsable(CaptureMethod::Portal)) {
        const QList<CaptureSource> offered = captureWindows(CaptureMethod::Portal);
        QVERIFY(offered.size() == 1 || offered.size() == 2);
        QCOMPARE(offered.first().kind, CaptureSource::Kind::Window);
        QVERIFY(isChosenByTheDesktop(offered.first()));
        QVERIFY(!isHandedOverByTheDesktop(offered.first()));
        QVERIFY(!offered.first().name.isEmpty());
        QVERIFY(offered.first().geometry.isEmpty());
        QCOMPARE(offered.first().processId, quint32(0));
        QCOMPARE(encodeSizeOf(offered.first(), 720), QSize());
        QVERIFY(offered.first().id != captureSources(CaptureMethod::Portal).value(0).id);
    }
    if (!captureMethodUsable(CaptureMethod::X11))
        QSKIP("only X11 lists the windows, and it does not show this desktop");
#ifdef ACHERON_TESTS_X11
    std::unique_ptr<TestWindow> own;
    std::unique_ptr<TestWindow> another;
    TestWindow::Look asAnother;
    if (const std::optional<QuietScreen> quiet = quietScreen()) {
        asAnother = windowLook(QSize(320, 240), quiet->cornerFor(QSize(400, 340)));
        asAnother.outOfTheWay = false;
        own = std::make_unique<TestWindow>(asAnother);
        QVERIFY(own->isOpen());
        asAnother.at = quiet->cornerFor(QSize(800, 340));
        asAnother.ours = false;
        asAnother.title = QStringLiteral("Acheron capture test %1").arg(QCoreApplication::applicationPid());
        another = std::make_unique<TestWindow>(asAnother);
        if (!TestWindow::aWindowManagerRuns(TestWindow::Display())) {
            own->setState(TestWindow::NORMAL_STATE);
            another->setState(TestWindow::NORMAL_STATE);
        }
        QTRY_VERIFY(own->isShown() && another->isShown());
        QTest::qWait(200);
    }

    const QList<CaptureSource> windows = captureWindows(CaptureMethod::X11);
    // window titles stay out of the log
    qInfo() << windows.size() << "windows listed";
    QStringList ids;
    for (const CaptureSource &window : windows) {
        QCOMPARE(window.kind, CaptureSource::Kind::Window);
        QVERIFY(window.id.startsWith(QStringLiteral("window:")));
        QVERIFY(!ids.contains(window.id));
        ids.append(window.id);
        QVERIFY(!window.name.isEmpty());
        QVERIFY(window.processId != static_cast<quint32>(QCoreApplication::applicationPid()));
        bool numbered = false;
        QVERIFY(window.id.mid(7).toUInt(&numbered) != 0 && numbered);
        if (another && window.id == another->sourceId()) {
            QCOMPARE(window.name, asAnother.title);
            QCOMPARE(window.geometry, another->insideOnScreen());
            QCOMPARE(window.processId, quint32(0));
            QCOMPARE(encodeSizeOf(window, std::nullopt), QSize(320, 240));
        }
    }
    if (own) {
        QVERIFY(!ids.contains(own->sourceId()));
        QVERIFY(ids.contains(another->sourceId()));
    }
#endif
#endif
}

void TestVideoEncode::aWindowHasAThumbnail_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    QTest::addColumn<bool>("scaledByWindows");
    QTest::newRow("a window that measures in the screen's pixels") << false;
    QTest::newRow("a window Windows scales") << true;
}

void TestVideoEncode::aWindowHasAThumbnail()
{
#ifdef Q_OS_WIN
    QFETCH(bool, scaledByWindows);
    using namespace Core::Video;
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const TestWindow window(TestWindow::Look{ QSize(480, 360), quiet->cornerFor(QSize(560, 460)), SHARED_COLOUR, true, scaledByWindows });
    QVERIFY(window.isOpen());
    QTest::qWait(300);

    CaptureSource source;
    source.id = window.sourceId();
    source.kind = CaptureSource::Kind::Window;
    const QSize bound(224, 126);
    QElapsedTimer timer;
    timer.start();
    const QImage thumbnail = captureThumbnail(source, bound);
    qInfo() << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
    QVERIFY(!thumbnail.isNull());
    QVERIFY(thumbnail.width() <= bound.width() && thumbnail.height() <= bound.height());
    QVERIFY(thumbnail.width() == bound.width() || thumbnail.height() == bound.height());
    QCOMPARE(thumbnail.format(), QImage::Format_RGB32);

    const auto isTheWindowsColour = [](QRgb pixel) { return qAbs(qRed(pixel) - PATCH_RED) < 12 && qAbs(qGreen(pixel) - PATCH_GREEN) < 12 && qAbs(qBlue(pixel) - PATCH_BLUE) < 12; };
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() / 2)));
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() - 4)));
    QVERIFY(isTheWindowsColour(thumbnail.pixel(thumbnail.width() - 4, thumbnail.height() / 2)));
    QVERIFY(!isTheWindowsColour(thumbnail.pixel(thumbnail.width() / 2, 2)));

    source.id = QStringLiteral("window:1");
    QVERIFY(captureThumbnail(source, bound).isNull());
#else
    QFETCH(bool, scaledByWindows);
    using namespace Core::Video;
    if (scaledByWindows)
        QSKIP("no window is scaled by Windows here");
    const QSize bound(224, 126);
    if (captureMethodUsable(CaptureMethod::Portal)) {
        QVERIFY(captureThumbnail(captureWindows(CaptureMethod::Portal).value(0), bound).isNull());
        CaptureSource handedOver;
        handedOver.kind = CaptureSource::Kind::Window;
        handedOver.id = QStringLiteral("pipewire:0");
        QVERIFY(isHandedOverByTheDesktop(handedOver));
        QVERIFY(captureThumbnail(handedOver, bound).isNull());
    }
    if (!captureMethodUsable(CaptureMethod::X11))
        QSKIP("only X11 has a thumbnail of a window, and it does not show this desktop");
#ifdef ACHERON_TESTS_X11
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const TestWindow window(windowLook(QSize(480, 360), quiet->cornerFor(QSize(560, 460))));
    QVERIFY(window.isOpen());
    QTRY_VERIFY(window.isShown());
    QTest::qWait(300);

    CaptureSource source = window.source();
    QElapsedTimer timer;
    timer.start();
    const QImage thumbnail = captureThumbnail(source, bound);
    qInfo() << "thumbnail" << thumbnail.size() << "in" << timer.elapsed() << "ms";
    QVERIFY(!thumbnail.isNull());
    QCOMPARE(thumbnail.size(), QSize(168, 126));
    QCOMPARE(thumbnail.format(), QImage::Format_RGB32);
    QCOMPARE(*reinterpret_cast<const quint32 *>(thumbnail.constScanLine(0)) >> 24, 0xFFu);

    QVERIFY(isColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() / 2), SHARED_COLOUR));
    QVERIFY(isColour(thumbnail.pixel(thumbnail.width() / 2, thumbnail.height() - 1), SHARED_COLOUR));
    QVERIFY(isColour(thumbnail.pixel(thumbnail.width() - 1, thumbnail.height() / 2), SHARED_COLOUR));
    QVERIFY(isColour(thumbnail.pixel(thumbnail.width() / 2, 0), SHARED_COLOUR));

    const QImage whole = captureThumbnail(source, QSize(1000, 1000));
    QCOMPARE(whole.size(), QSize(480, 360));
    QCOMPARE(reinterpret_cast<const quint32 *>(whole.constScanLine(180))[240], 0xFF000000u | static_cast<quint32>(PATCH_RED << 16 | PATCH_GREEN << 8 | PATCH_BLUE));

    source.id = QStringLiteral("window:1");
    QVERIFY(captureThumbnail(source, bound).isNull());
#endif
#endif
}

void TestVideoEncode::thePipelineSharesAWindow_data()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using Core::Video::CaptureMethod;
    QTest::addColumn<CaptureMethod>("method");
    QTest::addColumn<bool>("scaledByWindows");
    QTest::addColumn<int>("maxHeight");
    QTest::newRow("by graphics capture") << CaptureMethod::GraphicsCapture << false << 0;
    QTest::newRow("by graphics capture, a window Windows scales, scaled down") << CaptureMethod::GraphicsCapture << true << 360;
    QTest::newRow("by duplication") << CaptureMethod::DesktopDuplication << false << 0;
    QTest::newRow("by duplication, a window Windows scales, scaled down") << CaptureMethod::DesktopDuplication << true << 360;
#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QTest::newRow("handed over by PipeWire") << CaptureMethod::Portal << false << 0;
    QTest::newRow("handed over by PipeWire, scaled down") << CaptureMethod::Portal << false << 360;
#endif
#ifdef ACHERON_TESTS_X11
    QTest::newRow("grabbed from X11") << CaptureMethod::X11 << false << 0;
    QTest::newRow("grabbed from X11, scaled down") << CaptureMethod::X11 << false << 360;
    QTest::newRow(TINY_FROM_X11) << CaptureMethod::X11 << false << 0;
#endif
}

#ifdef ACHERON_TESTS_X11
namespace {

void theWindowIsSharedFromX11(int maxHeight)
{
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!captureMethodUsable(CaptureMethod::X11))
        QSKIP(NO_X11);
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first();

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const QPoint home = quiet->cornerFor(QSize(860, 700));
    TestWindow window(windowLook(QSize(640, 480), home));
    QVERIFY(window.isOpen());
    QTRY_VERIFY(window.isShown());
    QTest::qWait(200);
    const CaptureSource source = window.source();
    const QSize firstSize = encodeSizeFor(window.insideOnScreen().size(), limitOf(maxHeight));
    QCOMPARE(firstSize != window.insideOnScreen().size(), maxHeight != 0);
    QCOMPARE(encodeSizeOf(source, limitOf(maxHeight)), firstSize);

    setCaptureMethod(CaptureMethod::X11);
    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = limitOf(maxHeight);
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty() || record.closes > 0, "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.closes, 0);
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), firstSize);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(!record.frames[1]->keyframe);
    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[4]->rtpTimestamp) / 15.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8 && ticksPerFrame < TICKS_PER_FRAME * 1.25, qPrintable(QString::number(ticksPerFrame)));

    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    qInfo().noquote() << "captured as" << record.stats.last().encoderName;
    QVERIFY(record.stats.last().encoderName.endsWith(QStringLiteral(", from x11grab")));
    QVERIFY2(!record.stats.last().encoderName.contains(QStringLiteral(" (")), "pictures grabbed into memory reached the encoder some other way");

    // x11grab delivers a picture whether or not anything changed
    window.setAnimated(false);
    QTest::qWait(300);
    size_t before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileStill = record.frames.size() - before;
    QVERIFY2(whileStill >= FRAMERATE * 2 * 0.8 && whileStill <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileStill)));

    const QRect atHome = window.insideOnScreen();
    window.moveTo(home - QPoint(240, 160));
    QTRY_VERIFY(window.insideOnScreen().topLeft() != atHome.topLeft());
    before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileElsewhere = record.frames.size() - before;
    qInfo() << "frames in 2 s:" << whileStill << "of a window that was still," << whileElsewhere << "of one that had been moved";
    QVERIFY2(whileElsewhere >= FRAMERATE * 2 * 0.8 && whileElsewhere <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileElsewhere)));
    QCOMPARE(record.keyFrameFrom(1), record.frames.size());

    const size_t atRequest = record.frames.size();
    const qint64 askedAt = record.clock.elapsed();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(record.keyFrameFrom(atRequest) < record.frames.size(), 1500);
    const qint64 waited = record.arrivedAt[record.keyFrameFrom(atRequest)] - askedAt;
    qInfo() << "a key frame" << waited << "ms after it was asked for";
    QVERIFY(waited < 400);
    QTest::qWait(200);
    QCOMPARE(record.keyFrameFrom(record.keyFrameFrom(atRequest) + 1), record.frames.size());
    QCOMPARE(record.starts.size(), 1);
    QVERIFY(record.stalls.isEmpty());

    window.setAnimated(true);
    const size_t atResize = record.frames.size();
    window.resizeInside(maxHeight ? QSize(800, 400) : QSize(800, 600));
    QTRY_COMPARE(window.insideOnScreen().size(), maxHeight ? QSize(800, 400) : QSize(800, 600));
    const QSize secondSize = encodeSizeFor(window.insideOnScreen().size(), limitOf(maxHeight));
    QVERIFY(secondSize != firstSize);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), secondSize);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atResize + 20, 3000);
    QVERIFY(record.keyFrameFrom(atResize) < record.frames.size());
    QCOMPARE(encodeSizeOf(source, limitOf(maxHeight)), secondSize);

    const QRect inside = window.insideOnScreen();
    QTest::qWait(300);
    const size_t atCover = record.frames.size();
    size_t coveredUntil = 0;
    {
        const TestWindow cover(windowLook(inside.size() / 3, inside.center() - QPoint(inside.width() / 6, inside.height() / 6), COVERING_COLOUR, false));
        QVERIFY(cover.isOpen());
        QTRY_VERIFY2(isColour(onTheScreen(*quiet, inside.center()), COVERING_COLOUR), "the second window did not come to lie over the first");
        QTest::qWait(1400);
        QVERIFY(isColour(onTheScreen(*quiet, inside.center()), COVERING_COLOUR));
        coveredUntil = record.frames.size();
    }
    QVERIFY(coveredUntil >= atCover + 20);
    QVERIFY(record.keyFrameFrom(atCover) >= coveredUntil);
    QCOMPARE(record.starts.size(), 2);
    const Decoded covered = decodeAll(codec, record.framesBefore(coveredUntil));
    QCOMPARE(covered.errors, quint64(0));
    QCOMPARE(QSize(covered.width, covered.height), secondSize);
    QVERIFY2(looksLike(covered.middle, SHARED_COLOUR), "the middle of the picture is not the shared window's colour");
    QVERIFY2(!looksLike(covered.middle, COVERING_COLOUR), "what covered the window is in the picture");
    QVERIFY2(looksLike(covered.top, SHARED_COLOUR), "the top of the picture is not the inside of the window");
    qInfo() << "covered, the middle decodes off the window's colour by" << offItsOwnCoefficients(codec, covered, covered.middle, SHARED_COLOUR) << "; the stream says its colours are" << covered.colorspace;
    QVERIFY2(offItsOwnCoefficients(codec, covered, covered.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive while it was covered");

    QList<bool> stalls;
    QVERIFY(record.stalls.isEmpty());
    const bool managed = TestWindow::aWindowManagerRuns(TestWindow::Display());
    for (int pause = 0; pause < (managed ? 2 : 1); pause++) {
        if (pause == 0)
            window.hide();
        else
            window.minimise();
        stalls.append(true);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 2000);
        QTest::qWait(300);
        const size_t whilePaused = record.frames.size();
        QTest::qWait(700);
        QCOMPARE(record.frames.size(), whilePaused);
        QCOMPARE(encodeSizeOf(source, limitOf(maxHeight)), QSize());

        if (pause == 0)
            window.show();
        else
            restoreIfMinimised(source);
        stalls.append(false);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= whilePaused + 10, 3000);
        QCOMPARE(record.starts.size(), 2);
    }

    window.close();
    QTRY_COMPARE_WITH_TIMEOUT(record.closes, 1, 3000);
    QTest::qWait(300);
    const size_t afterClose = record.frames.size();
    QTest::qWait(300);
    QCOMPARE(record.frames.size(), afterClose);
    QCOMPARE(record.closes, 1);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.stalls, stalls);

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    std::atomic<int> blackPictures{ 0 };
    const Decoded decoded = decodeAll(codec, record.frames, [&blackPictures](const AVFrame *picture, int) {
        if (picture->data[0][(picture->height / 2) * picture->linesize[0] + picture->width / 2] < BLACK_AT_MOST)
            blackPictures++;
    });
    QCOMPARE(blackPictures.load(), 0);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), secondSize);
    QVERIFY2(looksLike(decoded.middle, SHARED_COLOUR), "the middle of the last picture is not the shared window's colour");
    QVERIFY2(looksLike(decoded.top, SHARED_COLOUR), "the top of the last picture is not the inside of the window");
    qInfo() << "uncovered, the middle decodes off the window's colour by" << offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive");
}

void aTinyWindowIsSharedFromX11OnBlack()
{
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!captureMethodUsable(CaptureMethod::X11))
        QSKIP(NO_X11);
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first();
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const TestWindow window(windowLook(TINY_WINDOW, quiet->cornerFor(QSize(400, 340)), SHARED_COLOUR, false));
    QVERIFY(window.isOpen());
    QTRY_COMPARE(window.insideOnScreen().size(), TINY_WINDOW);
    QCOMPARE(encodeSizeOf(window.source(), std::nullopt), SMALLEST_PICTURE);

    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = window.sourceId();
    settings.maxHeight = std::nullopt;
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty(), "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), SMALLEST_PICTURE);
    pipeline.stop();
    QTest::qWait(200);

    std::atomic<int> blackBeside{ 0 };
    std::atomic<int> blackBelow{ 0 };
    const Decoded decoded = decodeAll(codec, record.frames, [&](const AVFrame *picture, int) {
        blackBeside += picture->data[0][40 * picture->linesize[0] + picture->width - 20] < BLACK_AT_MOST ? 1 : 0;
        blackBelow += picture->data[0][(picture->height - 12) * picture->linesize[0] + 40] < BLACK_AT_MOST ? 1 : 0;
    });
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), SMALLEST_PICTURE);
    QCOMPARE(blackBeside.load(), decoded.pictures);
    QCOMPARE(blackBelow.load(), decoded.pictures);
    QVERIFY2(looksLike(decoded.top, SHARED_COLOUR), "the top of the picture is not the inside of the window");
}

} // namespace
#endif

void TestVideoEncode::thePipelineSharesAWindow()
{
#ifdef ACHERON_TESTS_X11
    {
        QFETCH(Core::Video::CaptureMethod, method);
        QFETCH(int, maxHeight);
        if (method == Core::Video::CaptureMethod::X11) {
            if (QByteArray(QTest::currentDataTag()) == TINY_FROM_X11)
                aTinyWindowIsSharedFromX11OnBlack();
            else
                theWindowIsSharedFromX11(maxHeight);
            return;
        }
    }
#endif
#ifdef Q_OS_WIN
    QFETCH(Core::Video::CaptureMethod, method);
    QFETCH(bool, scaledByWindows);
    QFETCH(int, maxHeight);
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (!windowCaptureAvailable() || !captureMethodUsable(method))
        QSKIP("this build or this Windows cannot capture a window this way");
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first();
    const bool duplicated = method == CaptureMethod::DesktopDuplication;
    const auto countsOverASecond = [](PipelineRecord &record, PathCounts *earlier, PathCounts *later) {
        for (PathCounts *counts : { earlier, later }) {
            const int seen = record.stats.size();
            if (!QTest::qWaitFor([&record, seen] { return record.stats.size() > seen; }, 2500))
                return false;
            *counts = pathCounts(record.stats.last());
        }
        return true;
    };

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    const QPoint home = quiet->cornerFor(QSize(860, 700));
    TestWindow window(TestWindow::Look{ QSize(640, 480), home, SHARED_COLOUR, true, scaledByWindows });
    QVERIFY(window.isOpen());
    QTest::qWait(200);
    const QSize firstSize = encodeSizeFor(window.insideOnScreen().size(), limitOf(maxHeight));
    QCOMPARE(firstSize != window.insideOnScreen().size(), maxHeight != 0);

    setCaptureMethod(method);
    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = window.sourceId();
    settings.maxHeight = limitOf(maxHeight);
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty() || record.closes > 0, "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.closes, 0);
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), firstSize);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(!record.frames[1]->keyframe);

    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[4]->rtpTimestamp) / 15.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8, qPrintable(QString::number(ticksPerFrame)));

    QList<std::pair<size_t, QString>> parts;
    const auto partBegins = [&parts, &record](const QString &name) { parts.append({ record.frames.size(), name }); };
    partBegins(QStringLiteral("started"));

    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    qInfo().noquote() << "captured as" << record.stats.last().encoderName;
    if (duplicated)
        QVERIFY(pathCounts(record.stats.last()).cutOut > 0);
    else
        QVERIFY(record.stats.last().encoderName.endsWith(QStringLiteral(", from gfxcapture")));
    const bool onCard = reachesTheEncoderOnTheCard(codec, maxHeight != 0, !duplicated);
    QCOMPARE(record.stats.last().encoderName.contains(onDevice(cardDevice())), onCard);

    partBegins(QStringLiteral("still"));
    window.setAnimated(false);
    QTest::qWait(300);
    size_t before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileStill = record.frames.size() - before;
    QVERIFY2(whileStill >= 3 && whileStill <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileStill)));

    partBegins(QStringLiteral("off the screen"));
    window.moveTo(QPoint(-6000, -6000));
    QTest::qWait(500);
    before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileAway = record.frames.size() - before;
    QVERIFY2(whileAway >= 3 && whileAway <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileAway)));
    partBegins(QStringLiteral("back on the screen"));
    window.moveTo(home);

    partBegins(QStringLiteral("cloaked"));
    window.setCloaked(true);
    QTest::qWait(700);
    before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileCloaked = record.frames.size() - before;
    qInfo() << "frames in 2 s:" << whileStill << "of a window that was still," << whileAway << "of one off the screen," << whileCloaked << "of one Windows had stopped drawing";
    QVERIFY2(whileCloaked >= 3 && whileCloaked <= FRAMERATE * 2 * 1.25, qPrintable(QString::number(whileCloaked)));

    const size_t atRequest = record.frames.size();
    const qint64 askedAt = record.clock.elapsed();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(record.keyFrameFrom(atRequest) < record.frames.size(), 1500);
    const qint64 waited = record.arrivedAt[record.keyFrameFrom(atRequest)] - askedAt;
    qInfo() << "a key frame" << waited << "ms after it was asked for";
    QVERIFY(waited < 400);
    QTest::qWait(200);
    QCOMPARE(record.keyFrameFrom(record.keyFrameFrom(atRequest) + 1), record.frames.size());
    partBegins(QStringLiteral("uncloaked"));
    window.setCloaked(false);
    QTest::qWait(500);
    QCOMPARE(record.starts.size(), 1);
    QVERIFY(record.stalls.isEmpty());

    partBegins(QStringLiteral("resized"));
    window.setAnimated(true);
    const size_t atResize = record.frames.size();
    window.resizeInside(maxHeight ? QSize(800, 400) : QSize(800, 600));
    const QSize secondSize = encodeSizeFor(window.insideOnScreen().size(), limitOf(maxHeight));
    QVERIFY(secondSize != firstSize);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), secondSize);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atResize + 20, 3000);
    QVERIFY(record.keyFrameFrom(atResize) < record.frames.size());

    const QRect inside = window.insideOnScreen();
    QTest::qWait(300);
    partBegins(QStringLiteral("covered"));
    const size_t atCover = record.frames.size();
    size_t coveredUntil = 0;
    {
        const TestWindow cover(TestWindow::Look{ inside.size() / 3, inside.center() - QPoint(inside.width() / 6, inside.height() / 6), COVERING_COLOUR, false });
        QVERIFY(cover.isOpen());
        QTest::qWait(400);
        PathCounts earlier;
        PathCounts later;
        QVERIFY(countsOverASecond(record, &earlier, &later));
        if (duplicated) {
            qInfo() << "covered:" << later.cutOut - earlier.cutOut << "pictures cut out of the screen and" << later.drawn - earlier.drawn << "drawn by the window in a second";
            QVERIFY(later.drawn > earlier.drawn);
            QCOMPARE(later.cutOut, earlier.cutOut);
        }
        coveredUntil = record.frames.size();
        partBegins(QStringLiteral("uncovered"));
    }
    QVERIFY(coveredUntil >= atCover + 20);
    QVERIFY(record.keyFrameFrom(atCover) >= coveredUntil);
    QCOMPARE(record.starts.size(), 2);
    const Decoded covered = decodeAll(codec, record.framesBefore(coveredUntil));
    QCOMPARE(covered.errors, quint64(0));
    QCOMPARE(QSize(covered.width, covered.height), secondSize);
    QVERIFY2(looksLike(covered.middle, SHARED_COLOUR), "the middle of the picture is not the shared window's colour");
    QVERIFY2(!looksLike(covered.middle, COVERING_COLOUR), "what covered the window is in the picture");
    QVERIFY2(looksLike(covered.top, SHARED_COLOUR), "the top of the picture is not the inside of the window");
    qInfo() << "covered, the middle decodes off the window's colour by" << offItsOwnCoefficients(covered, covered.middle, SHARED_COLOUR) << "; the stream says its colours are" << covered.colorspace;
    QVERIFY2(offItsOwnCoefficients(covered, covered.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive while it was covered");

    if (duplicated) {
        QTest::qWait(300);
        PathCounts earlier;
        PathCounts later;
        QVERIFY(countsOverASecond(record, &earlier, &later));
        qInfo() << "uncovered:" << later.cutOut - earlier.cutOut << "pictures cut out of the screen and" << later.drawn - earlier.drawn << "drawn by the window in a second";
        QVERIFY(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
        QVERIFY(later.drawn - earlier.drawn <= FRAMERATE / 6);
    }

    if (duplicated && window.reachPastAnOuterEdge(quiet->whole, 10)) {
        PathCounts earlier;
        PathCounts later;
        QTest::qWait(400);
        QVERIFY(countsOverASecond(record, &earlier, &later));
        QVERIFY(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
        QVERIFY(later.drawn - earlier.drawn <= FRAMERATE / 6);
        QVERIFY(window.reachPastAnOuterEdge(quiet->whole, 40));
        QTest::qWait(400);
        QVERIFY(countsOverASecond(record, &earlier, &later));
        QVERIFY(later.drawn > earlier.drawn);
        QCOMPARE(later.cutOut, earlier.cutOut);
        window.moveTo(home);
        QTest::qWait(400);
        QCOMPARE(record.starts.size(), 2);
    }

    QList<bool> stalls;
    QVERIFY(record.stalls.isEmpty());
    for (int pause = 0; pause < (duplicated ? 2 : 1); pause++) {
        partBegins(pause == 0 ? QStringLiteral("hidden") : QStringLiteral("kept out of captures"));
        if (pause == 0)
            window.hide();
        else if (!window.keepOutOfCaptures(true))
            break;
        stalls.append(true);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 2000);
        QTest::qWait(300);
        const size_t whilePaused = record.frames.size();
        QTest::qWait(700);
        QCOMPARE(record.frames.size(), whilePaused);

        partBegins(pause == 0 ? QStringLiteral("shown again") : QStringLiteral("let into captures again"));
        if (pause == 0)
            window.show();
        else
            window.keepOutOfCaptures(false);
        stalls.append(false);
        QTRY_COMPARE_WITH_TIMEOUT(record.stalls, stalls, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= whilePaused + 10, 3000);
        QCOMPARE(record.starts.size(), 2);
    }

    partBegins(QStringLiteral("closed"));
    window.close();
    QTRY_COMPARE_WITH_TIMEOUT(record.closes, 1, 3000);
    QTest::qWait(300);
    const size_t afterClose = record.frames.size();
    QTest::qWait(300);
    QCOMPARE(record.frames.size(), afterClose);
    QCOMPARE(record.closes, 1);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.stalls, stalls);

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    // graphics capture hands over an empty picture as a window is hidden, shown again and closed; sent on,
    // it shows black
    QMutex blackMutex;
    QStringList blackPictures;
    const Decoded decoded = decodeAll(codec, record.frames, [&blackMutex, &blackPictures, &parts](const AVFrame *picture, int index) {
        if (picture->data[0][(picture->height / 2) * picture->linesize[0] + picture->width / 2] >= BLACK_AT_MOST)
            return;
        QString part;
        for (const auto &[from, name] : parts) {
            if (from <= static_cast<size_t>(index))
                part = QStringLiteral("%1 after it was %2").arg(static_cast<size_t>(index) - from).arg(name);
        }
        const QMutexLocker locker(&blackMutex);
        blackPictures.append(QStringLiteral("picture %1 (%2)").arg(index).arg(part));
    });
    if (!duplicated)
        QVERIFY2(blackPictures.isEmpty(), qPrintable(QStringLiteral("black: ") + blackPictures.join(QStringLiteral(", "))));
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), secondSize);
    QVERIFY2(looksLike(decoded.middle, SHARED_COLOUR), "the middle of the last picture is not the shared window's colour");
    QVERIFY2(looksLike(decoded.top, SHARED_COLOUR), "the top of the last picture is not the inside of the window");
    qInfo() << "uncovered, the middle decodes off the window's colour by" << offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY2(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the window's colour did not survive");
    QCOMPARE(record.stats.last().encoderName.contains(onDevice(cardDevice())), onCard);
#elif defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    QFETCH(Core::Video::CaptureMethod, method);
    QFETCH(int, maxHeight);
    using namespace Core::Video;
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.isEmpty())
        QSKIP("this machine has no encoder");
    if (method != CaptureMethod::Portal)
        QSKIP("this is one of Windows' ways of capturing a window");
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first();

    StandInDesktop window(STAND_IN_WINDOW);
    if (!window.isOpen())
        QSKIP(NO_PIPEWIRE);
    const QSize firstSize = encodeSizeFor(STAND_IN_WINDOW, limitOf(maxHeight));
    QCOMPARE(firstSize != STAND_IN_WINDOW, maxHeight != 0);
    QCOMPARE(encodeSizeOf(window.source(), limitOf(maxHeight)), firstSize);

    setCaptureMethod(method);
    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = window.source().id;
    settings.maxHeight = limitOf(maxHeight);
    settings.framerate = FRAMERATE;
    settings.codec = codec;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty() || record.closes > 0, "fewer than 20 frames in 4 s", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.closes, 0);
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first(), firstSize);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(!record.frames[1]->keyframe);
    for (size_t i = 1; i < 20; i++)
        QVERIFY(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
    const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[4]->rtpTimestamp) / 15.0;
    QVERIFY2(ticksPerFrame > TICKS_PER_FRAME * 0.8 && ticksPerFrame < TICKS_PER_FRAME * 1.25, qPrintable(QString::number(ticksPerFrame)));

    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    qInfo().noquote() << "captured as" << record.stats.last().encoderName;
    QVERIFY(record.stats.last().encoderName.endsWith(QStringLiteral(", from pipewire memfd BGRx")));

    window.setAnimated(false);
    QTest::qWait(300);
    const size_t before = record.frames.size();
    QTest::qWait(2000);
    const size_t whileStill = record.frames.size() - before;
    qInfo() << "frames in 2 s of a window that was still:" << whileStill;
    QVERIFY2(whileStill >= 3 && whileStill <= 5, qPrintable(QString::number(whileStill)));

    const size_t atRequest = record.frames.size();
    const qint64 askedAt = record.clock.elapsed();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(record.keyFrameFrom(atRequest) < record.frames.size(), 1500);
    const qint64 waited = record.arrivedAt[record.keyFrameFrom(atRequest)] - askedAt;
    qInfo() << "a key frame" << waited << "ms after it was asked for";
    QVERIFY(waited < 400);
    QTest::qWait(200);
    QCOMPARE(record.keyFrameFrom(record.keyFrameFrom(atRequest) + 1), record.frames.size());
    QCOMPARE(record.starts.size(), 1);
    QVERIFY(record.stalls.isEmpty());

    window.setAnimated(true);
    QTest::qWait(300);
    const size_t atResize = record.frames.size();
    const QSize resized = maxHeight ? QSize(800, 400) : QSize(800, 600);
    const QSize secondSize = encodeSizeFor(resized, limitOf(maxHeight));
    QVERIFY(secondSize != firstSize);
    window.resize(resized);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), secondSize);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atResize + 20, 3000);
    QVERIFY(record.keyFrameFrom(atResize) < record.frames.size());
    QCOMPARE(encodeSizeOf(window.source(), limitOf(maxHeight)), secondSize);

    window.close();
    QTRY_COMPARE_WITH_TIMEOUT(record.closes, 1, 3000);
    QTest::qWait(300);
    const size_t afterClose = record.frames.size();
    QTest::qWait(300);
    QCOMPARE(record.frames.size(), afterClose);
    QCOMPARE(record.closes, 1);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QVERIFY(record.stalls.isEmpty());
    QCOMPARE(record.starts.size(), 2);

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(QSize(decoded.width, decoded.height), secondSize);
    QVERIFY2(looksLike(decoded.middle, SHARED_COLOUR), "the middle of the last picture is not the colour handed over");
    QVERIFY2(looksLike(decoded.top, SHARED_COLOUR), "the top of the last picture is not the colour handed over");
    qInfo() << "the middle decodes off the colour handed over by" << offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) << "; the stream says its colours are" << decoded.colorspace;
    QVERIFY2(offItsOwnCoefficients(codec, decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES, "the colour handed over did not survive");
#else
    QSKIP("this build captures no window");
#endif
}

#ifdef ACHERON_TESTS_X11
namespace {

void whatAv1WouldPadIsHeldBackOfAWindowFromX11()
{
    using namespace Core::Video;
    using Core::Media::codedSize;
    const QString encoderName = Core::Media::videoEncoderFor(VideoCodec::AV1);
    const QSize padded(700, 500);
    const QSize exact(640, 480);
    if (encoderName.isEmpty() || codedSize(encoderName, padded) == padded)
        QSKIP("this machine's AV1 encoder, if it has one, codes every size as it is");
    QVERIFY(!Core::Media::paddingIsAcceptable(padded, codedSize(encoderName, padded)));
    QCOMPARE(codedSize(encoderName, exact), exact);

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    TestWindow window(windowLook(padded, quiet->cornerFor(QSize(860, 700))));
    QVERIFY(window.isOpen());
    QTRY_COMPARE(window.insideOnScreen().size(), padded);
    const CaptureSource source = window.source();
    QCOMPARE(encodeSizeOf(source, std::nullopt), padded);
    QCOMPARE(encodeSizeOf(source, 360), encodeSizeFor(padded, 360));

    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = std::nullopt;
    settings.framerate = FRAMERATE;
    settings.codec = VideoCodec::AV1;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(!record.starts.isEmpty() || !record.failures.isEmpty(), "the size was not reported", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first(), padded);
    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    QTest::qWait(500);
    QVERIFY(record.frames.empty());
    QVERIFY2(record.stats.last().encoderName.startsWith(QStringLiteral("none while AV1")), qPrintable(record.stats.last().encoderName));
    QVERIFY2(record.stats.last().encoderName.endsWith(QStringLiteral(", from x11grab")), qPrintable(record.stats.last().encoderName));
    QVERIFY(record.stats.last().capturedFps > 0);
    QCOMPARE(record.stats.last().framesEncoded, quint64(0));

    window.resizeInside(exact);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), exact);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= 10, 3000);
    QVERIFY(record.frames[0]->keyframe);
    pipeline.stop();
    QTest::qWait(200);

    const Decoded decoded = decodeAll(VideoCodec::AV1, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(QSize(decoded.width, decoded.height), exact);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
}

} // namespace
#endif

void TestVideoEncode::thePipelineHoldsBackWhatItsEncoderWouldPad()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef ACHERON_TESTS_X11
    if (Core::Video::captureMethodUsable(Core::Video::CaptureMethod::X11)) {
        whatAv1WouldPadIsHeldBackOfAWindowFromX11();
        return;
    }
#endif
#ifdef Q_OS_WIN
    using namespace Core::Video;
    using Core::Media::codedSize;
    const QString encoderName = Core::Media::videoEncoderFor(VideoCodec::AV1);
    const QSize padded(700, 500);
    const QSize exact(640, 480);
    if (encoderName.isEmpty() || codedSize(encoderName, padded) == padded)
        QSKIP("this machine's AV1 encoder, if it has one, codes every size as it is");
    if (!windowCaptureAvailable())
        QSKIP("this build or this Windows cannot capture a window");
    QVERIFY(!Core::Media::paddingIsAcceptable(padded, codedSize(encoderName, padded)));
    QCOMPARE(codedSize(encoderName, exact), exact);

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    TestWindow window(TestWindow::Look{ padded, quiet->cornerFor(QSize(860, 700)) });
    QVERIFY(window.isOpen());
    QTest::qWait(200);
    QCOMPARE(window.insideOnScreen().size(), padded);

    CaptureSource source;
    source.id = window.sourceId();
    source.kind = CaptureSource::Kind::Window;
    QCOMPARE(encodeSizeOf(source, std::nullopt), padded);
    QCOMPARE(encodeSizeOf(source, 360), encodeSizeFor(padded, 360));

    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = std::nullopt;
    settings.framerate = FRAMERATE;
    settings.codec = VideoCodec::AV1;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(!record.starts.isEmpty() || !record.failures.isEmpty(), "the size was not reported", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first(), padded);
    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    QTest::qWait(500);
    QVERIFY(record.frames.empty());
    QVERIFY2(record.stats.last().encoderName.startsWith(QStringLiteral("none while AV1")), qPrintable(record.stats.last().encoderName));
    QVERIFY(record.stats.last().capturedFps > 0);
    QCOMPARE(record.stats.last().framesEncoded, quint64(0));

    window.resizeInside(exact);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), exact);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= 10, 3000);
    QVERIFY(record.frames[0]->keyframe);
    pipeline.stop();
    QTest::qWait(200);

    const Decoded decoded = decodeAll(VideoCodec::AV1, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(QSize(decoded.width, decoded.height), exact);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
#elif defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
    using namespace Core::Video;
    using Core::Media::codedSize;
    const QString encoderName = Core::Media::videoEncoderFor(VideoCodec::AV1);
    const QSize padded(700, 500);
    const QSize exact(640, 480);
    if (encoderName.isEmpty() || codedSize(encoderName, padded) == padded)
        QSKIP("this machine's AV1 encoder, if it has one, codes every size as it is");
    QVERIFY(!Core::Media::paddingIsAcceptable(padded, codedSize(encoderName, padded)));
    QCOMPARE(codedSize(encoderName, exact), exact);

    StandInDesktop window(padded);
    if (!window.isOpen())
        QSKIP(NO_PIPEWIRE);
    QCOMPARE(encodeSizeOf(window.source(), std::nullopt), padded);
    QCOMPARE(encodeSizeOf(window.source(), 360), encodeSizeFor(padded, 360));

    ScreenPipeline pipeline;
    PipelineRecord record(pipeline);
    ScreenSettings settings;
    settings.sourceId = window.source().id;
    settings.maxHeight = std::nullopt;
    settings.framerate = FRAMERATE;
    settings.codec = VideoCodec::AV1;
    settings.bitrateBps = 2'500'000;
    settings.maxBitrateBps = 4'000'000;
    pipeline.start(settings);

    QTRY_VERIFY2_WITH_TIMEOUT(!record.starts.isEmpty() || !record.failures.isEmpty(), "the size was not reported", 4000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first(), padded);
    QTRY_VERIFY_WITH_TIMEOUT(!record.stats.isEmpty(), 2500);
    QTest::qWait(500);
    QVERIFY(record.frames.empty());
    QVERIFY2(record.stats.last().encoderName.startsWith(QStringLiteral("none while AV1")), qPrintable(record.stats.last().encoderName));
    QVERIFY(record.stats.last().capturedFps > 0);
    QCOMPARE(record.stats.last().framesEncoded, quint64(0));

    window.resize(exact);
    QTRY_VERIFY2_WITH_TIMEOUT(record.starts.size() == 2, "the encoded size did not follow the window", 4000);
    QCOMPARE(record.starts.last(), exact);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= 10, 3000);
    QVERIFY(record.frames[0]->keyframe);
    pipeline.stop();
    QTest::qWait(200);

    const Decoded decoded = decodeAll(VideoCodec::AV1, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(QSize(decoded.width, decoded.height), exact);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
#else
    QSKIP("this build captures no window");
#endif
}

// AMD's AV1 Media Foundation encoder can hang the process at exit once a probe thread was left inside
// it, so on Windows this ends without unloading anything
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    TestVideoEncode test;
    QTEST_SET_MAIN_SOURCE_PATH
    const int failed = QTest::qExec(&test, argc, argv);
#ifdef Q_OS_WIN
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(failed));
#endif
    return failed;
}

#include "tst_VideoEncode.moc"
