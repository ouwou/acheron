#include "Core/Media/VideoDecoder.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/RunningAverage.hpp"

#include <QElapsedTimer>
#include <QSettings>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <span>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace Acheron {
namespace Core {
namespace Media {

namespace {

using Discord::Voice::VideoCodec;

const char *const HARDWARE_DECODE_KEY = "video/hardware_decode";

// Direct3D 11 doesnt decode video before Windows 8, hence DXVA2
#if defined(Q_OS_WIN)
constexpr std::array HARDWARE_DEVICES = { AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2 };
#elif defined(Q_OS_MACOS)
constexpr std::array HARDWARE_DEVICES = { AV_HWDEVICE_TYPE_VIDEOTOOLBOX };
#else
constexpr std::array HARDWARE_DEVICES = { AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA };
#endif

constexpr int HARDWARE_FAILURES_TOLERATED = 1;
std::array<std::atomic<int>, Discord::Voice::VIDEO_CODEC_COUNT> hardwareFailures = {};
constexpr std::chrono::seconds HARDWARE_RETRY_AFTER(60);

std::atomic<int> &hardwareFailuresOf(VideoCodec codec)
{
    return hardwareFailures[static_cast<size_t>(codec)];
}

// ffmpeg's own AV1 decoder only works through a hardware accelerator
const AVCodec *findSoftwareDecoder(VideoCodec codec)
{
    static constexpr std::array<const char *, 2> AV1 = { "libdav1d", "libaom-av1" };
    static constexpr std::array<const char *, 1> H265 = { "hevc" };
    static constexpr std::array<const char *, 1> H264 = { "h264" };
    static constexpr std::array<const char *, 2> VP8 = { "vp8", "libvpx" };

    std::span<const char *const> names;
    switch (codec) {
    case VideoCodec::AV1:
        names = AV1;
        break;
    case VideoCodec::H265:
        names = H265;
        break;
    case VideoCodec::H264:
        names = H264;
        break;
    case VideoCodec::VP8:
        names = VP8;
        break;
    case VideoCodec::Unknown:
        break;
    }

    for (const char *name : names) {
        if (const AVCodec *decoder = avcodec_find_decoder_by_name(name))
            return decoder;
    }
    return nullptr;
}

// only ffmpeg's own decoders take a hardware device
const AVCodec *findHardwareCapableDecoder(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::AV1:
        return avcodec_find_decoder_by_name("av1");
    case VideoCodec::H265:
        return avcodec_find_decoder_by_name("hevc");
    case VideoCodec::H264:
        return avcodec_find_decoder_by_name("h264");
    case VideoCodec::VP8:
        return avcodec_find_decoder_by_name("vp8");
    case VideoCodec::Unknown:
        break;
    }
    return nullptr;
}

AVPixelFormat hardwareFormat(const AVCodec *decoder, AVHWDeviceType type)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config)
            return AV_PIX_FMT_NONE;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type)
            return config->pix_fmt;
    }
}

AVPixelFormat chooseFormat(AVCodecContext *context, const AVPixelFormat *formats)
{
    const int wanted = *static_cast<const int *>(context->opaque);
    const AVPixelFormat *software = nullptr;
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; format++) {
        if (*format == wanted)
            return *format;
        const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(*format);
        if (!software && description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL))
            software = format;
    }
    return software ? *software : AV_PIX_FMT_NONE;
}

AVCodecContext *openSoftwareDecoder(const AVCodec *decoder, int threads = 1)
{
    AVCodecContext *context = avcodec_alloc_context3(decoder);
    if (!context)
        return nullptr;

    // frame threading holds back a frame per thread
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_type = FF_THREAD_SLICE;
    context->thread_count = std::clamp(threads, 1, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
    av_opt_set_int(context, "max_frame_delay", 1, AV_OPT_SEARCH_CHILDREN);

    if (avcodec_open2(context, decoder, nullptr) < 0) {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

bool softwareDecoderOpens(VideoCodec codec)
{
    const AVCodec *decoder = findSoftwareDecoder(codec);
    AVCodecContext *context = decoder ? openSoftwareDecoder(decoder) : nullptr;
    if (!context)
        return false;
    avcodec_free_context(&context);
    return true;
}

// one shared device per kind: one per decoder costs graphics memory per stream and a visible pause as
// each stream starts
AVBufferRef *sharedHardwareDevice(AVHWDeviceType type)
{
    struct Devices
    {
        ~Devices()
        {
            for (auto &[kind, device] : byType)
                av_buffer_unref(&device);
        }
        std::mutex mutex;
        std::map<AVHWDeviceType, AVBufferRef *> byType;
    };
    static Devices devices;

    std::lock_guard lock(devices.mutex);
    auto it = devices.byType.find(type);
    if (it == devices.byType.end()) {
        AVBufferRef *device = nullptr;
        if (!hardwareDeviceCanBeAskedFor(type) || av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0)
            device = nullptr;
        it = devices.byType.emplace(type, device).first;
    }
    return it->second ? av_buffer_ref(it->second) : nullptr;
}

bool hardwareDecoderExists(VideoCodec codec)
{
    const AVCodec *decoder = findHardwareCapableDecoder(codec);
    if (!decoder)
        return false;

    for (const AVHWDeviceType type : HARDWARE_DEVICES) {
        if (hardwareFormat(decoder, type) == AV_PIX_FMT_NONE)
            continue;
        AVBufferRef *device = sharedHardwareDevice(type);
        if (!device)
            continue;
        av_buffer_unref(&device);
        return true;
    }
    return false;
}

} // namespace

bool hardwareDecodingEnabled()
{
    return QSettings().value(HARDWARE_DECODE_KEY, true).toBool();
}

void setHardwareDecodingEnabled(bool enabled)
{
    QSettings().setValue(HARDWARE_DECODE_KEY, enabled);
}

QList<Discord::Voice::VideoCodec> decodableVideoCodecs()
{
    static const QList<VideoCodec> codecs = [] {
        QList<VideoCodec> found;
        for (VideoCodec codec : Discord::Voice::VIDEO_CODECS_BEST_FIRST) {
            if (softwareDecoderOpens(codec) || (hardwareDecodingEnabled() && hardwareDecoderExists(codec)))
                found.append(codec);
        }
        return found;
    }();
    return codecs;
}

VideoDecoder::VideoDecoder(bool allowHardware, int softwareThreads)
    : hardwareAllowed(allowHardware), softwareThreads(softwareThreads), thread([this] { run(); })
{
}

VideoDecoder::~VideoDecoder()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    thread.join();
}

void VideoDecoder::setSink(std::shared_ptr<VideoFrameSink> sink)
{
    std::lock_guard lock(mutex);
    frameSink = std::move(sink);
}

void VideoDecoder::setKeyframeRequester(std::function<void()> requester)
{
    std::lock_guard lock(mutex);
    keyframeRequester = std::move(requester);
}

void VideoDecoder::setCodecFailureHandler(std::function<void(Discord::Voice::VideoCodec)> handler)
{
    std::lock_guard lock(mutex);
    codecFailureHandler = std::move(handler);
}

VideoDecoder::Stats VideoDecoder::stats() const
{
    std::lock_guard lock(mutex);
    Stats now = counters;
    if (!queue.empty())
        now.backlogMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - queue.front().queuedAt).count());
    return now;
}

void VideoDecoder::push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame)
{
    bool overflowed = false;
    std::function<void()> requester;
    {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        const bool tooOld = !queue.empty() && now - queue.front().queuedAt > std::chrono::milliseconds(MAX_QUEUE_AGE_MS);
        if (tooOld || static_cast<int>(queue.size()) >= MAX_QUEUED_FRAMES) {
            counters.dropped += queue.size();
            counters.queuesDiscarded++;
            queue.clear();
            queueWasDiscarded = true;
            overflowed = true;
            requester = keyframeRequester;
        }
        queue.push_back({ codec, std::move(frame), now });
    }
    wake.notify_one();

    if (overflowed && requester)
        requester();
}

void VideoDecoder::run()
{
    decoded = av_frame_alloc();
    downloaded = av_frame_alloc();
    packet = av_packet_alloc();

    for (;;) {
        Queued queued;
        bool framesWereSkipped = false;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping)
                break;
            queued = std::move(queue.front());
            queue.pop_front();
            framesWereSkipped = std::exchange(queueWasDiscarded, false);
        }
        if (framesWereSkipped)
            startOverAtNextKeyframe();
        decode(queued);
    }

    close();
    av_frame_free(&decoded);
    av_frame_free(&downloaded);
    av_packet_free(&packet);
}

bool VideoDecoder::open(Discord::Voice::VideoCodec codec)
{
    close();

    const bool ruledOut = hardwareRuledOutUntil && std::chrono::steady_clock::now() < *hardwareRuledOutUntil;
    const bool tryHardware = hardwareAllowed && !ruledOut && hardwareFailuresOf(codec) <= HARDWARE_FAILURES_TOLERATED;
    if (!(tryHardware && openHardware(codec)) && !openSoftware(codec)) {
        qCWarning(LogVideo) << "No decoder could be opened for" << Discord::Voice::videoCodecName(codec);
        return false;
    }

    openCodec = codec;
    awaitingKeyframe = true;
    framesSinceOpen = 0;
    hardwareProven = false;
    describeDecoder();
    return true;
}

bool VideoDecoder::openHardware(Discord::Voice::VideoCodec codec)
{
    const AVCodec *decoder = findHardwareCapableDecoder(codec);
    if (!decoder)
        return false;

    for (const AVHWDeviceType type : HARDWARE_DEVICES) {
        const AVPixelFormat format = hardwareFormat(decoder, type);
        if (format == AV_PIX_FMT_NONE)
            continue;

        AVBufferRef *device = sharedHardwareDevice(type);
        if (!device)
            continue;

        AVCodecContext *candidate = avcodec_alloc_context3(decoder);
        if (!candidate) {
            av_buffer_unref(&device);
            continue;
        }
        hardwarePixelFormat = format;
        candidate->opaque = &hardwarePixelFormat;
        candidate->get_format = chooseFormat;
        candidate->hw_device_ctx = av_buffer_ref(device);
        // drivers under-report the levels they decode; ffmpeg's advice is to go by whether the stream decodes
        candidate->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        candidate->flags |= AV_CODEC_FLAG_LOW_DELAY;
        candidate->thread_count = 1;
        candidate->extra_hw_frames = EXTRA_HARDWARE_FRAMES;
        if (avcodec_open2(candidate, decoder, nullptr) < 0) {
            avcodec_free_context(&candidate);
            av_buffer_unref(&device);
            continue;
        }

        context = candidate;
        hardwareDevice = device;
        hardwareName = av_hwdevice_get_type_name(type);
        return true;
    }

    hardwarePixelFormat = -1;
    return false;
}

bool VideoDecoder::openSoftware(Discord::Voice::VideoCodec codec)
{
    const AVCodec *decoder = findSoftwareDecoder(codec);
    context = decoder ? openSoftwareDecoder(decoder, softwareThreads) : nullptr;
    return context != nullptr;
}

void VideoDecoder::close()
{
    avcodec_free_context(&context);
    av_buffer_unref(&hardwareDevice);
    hardwarePixelFormat = -1;
    hardwareName = nullptr;
    openCodec = Discord::Voice::VideoCodec::Unknown;
}

void VideoDecoder::describeDecoder()
{
    QString name = context ? QString::fromUtf8(context->codec->name) : QString();
    if (hardwareDevice && hardwareName)
        name += QStringLiteral(" (%1)").arg(QString::fromUtf8(hardwareName));

    std::lock_guard lock(mutex);
    counters.decoderName = name;
    counters.hardware = hardwareDevice != nullptr;
}

void VideoDecoder::startOverAtNextKeyframe()
{
    if (context)
        avcodec_flush_buffers(context);
    awaitingKeyframe = true;
}

void VideoDecoder::reportUndecodableFrame()
{
    awaitingKeyframe = true;

    std::function<void()> requester;
    {
        std::lock_guard lock(mutex);
        counters.errors++;
        requester = keyframeRequester;
    }
    if (requester)
        requester();
}

void VideoDecoder::decode(Queued &queued)
{
    if (queued.codec != openCodec && queued.codec == unopenableCodec) {
        std::lock_guard lock(mutex);
        counters.dropped++;
        return;
    }
    if (queued.codec != openCodec && !open(queued.codec)) {
        unopenableCodec = queued.codec;
        std::function<void(Discord::Voice::VideoCodec)> handler;
        {
            std::lock_guard lock(mutex);
            handler = codecFailureHandler;
        }
        if (handler)
            handler(queued.codec);
        return;
    }
    unopenableCodec.reset();

    const bool hardwareMayBeTried = hardwareRuledOutUntil && std::chrono::steady_clock::now() >= *hardwareRuledOutUntil;
    if (hardwareMayBeTried && queued.frame.keyframe && !hardwareDevice) {
        hardwareRuledOutUntil.reset();
        open(openCodec);
    }

    if (awaitingKeyframe && !queued.frame.keyframe) {
        std::lock_guard lock(mutex);
        counters.dropped++;
        return;
    }

    const int size = static_cast<int>(queued.frame.data.size());
    queued.frame.data.resize(queued.frame.data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    if (decodeWithOpenDecoder(queued.frame, size))
        return;

    const bool hardwareOnTrial = hardwareDevice && framesSinceOpen < HARDWARE_TRIAL_FRAMES;
    if (!hardwareOnTrial) {
        startOverAtNextKeyframe();
        reportUndecodableFrame();
        return;
    }

    const Discord::Voice::VideoCodec codec = openCodec;
    const QString hardware = QString::fromUtf8(hardwareName);
    const bool wasProven = hardwareProven;
    hardwareRuledOutUntil = std::chrono::steady_clock::now() + HARDWARE_RETRY_AFTER;
    if (open(codec) && queued.frame.keyframe) {
        if (decodeWithOpenDecoder(queued.frame, size)) {
            qCWarning(LogVideo) << "Hardware decoding of" << Discord::Voice::videoCodecName(codec) << "through" << hardware << "failed on a frame software decodes; using software";
            if (!wasProven)
                hardwareFailuresOf(codec)++;
            return;
        }
        hardwareRuledOutUntil.reset();
        close();
    }
    reportUndecodableFrame();
}

bool VideoDecoder::decodeWithOpenDecoder(const Discord::Voice::EncodedVideoFrame &paddedFrame, int size)
{
    QElapsedTimer timer;
    timer.start();

    packet->data = const_cast<uint8_t *>(paddedFrame.data.data());
    packet->size = size;
    packet->pts = paddedFrame.rtpTimestamp;
    if (paddedFrame.keyframe)
        packet->flags |= AV_PKT_FLAG_KEY;

    const int sent = avcodec_send_packet(context, packet);
    av_packet_unref(packet);
    if (sent < 0)
        return false;
    awaitingKeyframe = false;

    std::shared_ptr<VideoFrameSink> sink;
    {
        std::lock_guard lock(mutex);
        sink = frameSink;
    }

    for (;;) {
        const int received = avcodec_receive_frame(context, decoded);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
            return true;
        if (received < 0)
            return false;

        const AVFrame *output = decoded;
        if (hardwareDevice) {
            // a decoder that turned the hardware format down decodes in software without threads
            bool failed = decoded->format != hardwarePixelFormat;
            if (!failed && !(sink && sink->acceptsHardwareFrames(decoded->format))) {
                av_frame_unref(downloaded);
                failed = av_hwframe_transfer_data(downloaded, decoded, 0) < 0;
                av_frame_copy_props(downloaded, decoded);
                output = downloaded;
            }
            if (failed) {
                av_frame_unref(decoded);
                framesSinceOpen = 0;
                return false;
            }
        }

        if (sink)
            sink->present(output);
        framesSinceOpen++;
        hardwareProven = hardwareProven || (hardwareDevice && framesSinceOpen >= HARDWARE_TRIAL_FRAMES);

        {
            std::lock_guard lock(mutex);
            counters.decoded++;
            counters.width = output->width;
            counters.height = output->height;
            const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            counters.averageDecodeMs = withSample(counters.averageDecodeMs, elapsedMs, counters.decoded);
        }
        av_frame_unref(decoded);
    }
}

} // namespace Media
} // namespace Core
} // namespace Acheron
