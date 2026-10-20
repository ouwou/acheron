#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "Discord/Voice/VideoReceiver.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace Acheron {
namespace Core {
namespace Media {

// Whether decoders try the graphics card first. A setting; on unless the user turns it off.
[[nodiscard]] bool hardwareDecodingEnabled();
void setHardwareDecodingEnabled(bool enabled);

// The video codecs this machine can decode, in the order the official client prefers them:
// AV1, H265, H264, VP8. A codec counts when a software decoder for it opens, or when the
// graphics card offers one. Probed once.
[[nodiscard]] QList<Discord::Voice::VideoCodec> decodableVideoCodecs();

// The same as a SELECT_PROTOCOL codec list: payload types are positional. Every entry is offered
// for decoding, and those in `encodable` for sending as well.
[[nodiscard]] QList<Discord::Voice::Codec> offeredVideoCodecs(const QList<Discord::Voice::VideoCodec> &encodable = {});

// Where decoded frames go. present() runs on the decoder's thread and must not keep the frame
// past the call without taking its own reference. Frames are in system memory, as planar
// YUV from a software decoder or NV12 / P010 read back from a hardware one, unless the sink
// says it takes a hardware decoder's frames as they are.
class VideoFrameSink
{
public:
    virtual ~VideoFrameSink() = default;
    virtual void present(const AVFrame *frame) = 0;
    // Whether frames of this hardware pixel format (an AVPixelFormat) can be presented still
    // on the graphics card, which saves reading them back. Asked for every frame, on the
    // decoder's thread. A sink that holds on to such a frame holds one of the decoder's
    // surfaces, of which EXTRA_HARDWARE_FRAMES are set aside.
    [[nodiscard]] virtual bool acceptsHardwareFrames(int format) const
    {
        Q_UNUSED(format);
        return false;
    }
};

// Decodes one remote video on its own thread. Frames may be pushed from any thread; when the
// decoder falls behind or meets a frame it cannot use, it drops to the next key frame and asks
// for one. A hardware decoder that fails at the start is replaced by a software one, which is
// given the same frame: only if software decodes it does the failure count against the
// hardware for later decoders.
class VideoDecoder
{
public:
    struct Stats
    {
        // The decoder in use, with the hardware device in brackets when there is one.
        QString decoderName;
        int width = 0;
        int height = 0;
        quint64 decoded = 0;
        quint64 dropped = 0;
        quint64 errors = 0;
        quint64 queuesDiscarded = 0;
        // How long the oldest frame still waiting to be decoded has waited.
        int backlogMs = 0;
        double averageDecodeMs = 0;
        bool hardware = false;
    };

    // Hardware decoding follows the setting unless the caller says otherwise. `softwareThreads`
    // is how many threads a software decoder may use: a camera's small picture needs one,
    // and every decoder asking for the most adds up in a call full of cameras.
    explicit VideoDecoder(bool allowHardware = hardwareDecodingEnabled(), int softwareThreads = MAX_SOFTWARE_THREADS);
    ~VideoDecoder();

    void setSink(std::shared_ptr<VideoFrameSink> sink);
    // Called on the decoder's thread.
    void setKeyframeRequester(std::function<void()> requester);
    // Called on the decoder's thread when no decoder at all could be opened for a codec.
    void setCodecFailureHandler(std::function<void(Discord::Voice::VideoCodec)> handler);

    void push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame);

    [[nodiscard]] Stats stats() const;

    // How far behind the decoder may get before it gives up on catching up: a second and a
    // half, whatever the stream's frame rate, and never more frames than this. A decoder with
    // time to spare works a shorter backlog off within moments and loses nothing by it, where
    // starting over costs a key frame every time (measured on a 1080p60 stream).
    static constexpr int MAX_QUEUE_AGE_MS = 1500;
    static constexpr int MAX_QUEUED_FRAMES = 90;
    static constexpr int MAX_SOFTWARE_THREADS = 4;
    // A hardware decoder that errs before it has produced this many frames is given up on.
    static constexpr int HARDWARE_TRIAL_FRAMES = 30;
    static constexpr int EXTRA_HARDWARE_FRAMES = 4;

private:
    struct Queued
    {
        Discord::Voice::VideoCodec codec;
        Discord::Voice::EncodedVideoFrame frame;
        std::chrono::steady_clock::time_point queuedAt;
    };

    void run();
    bool open(Discord::Voice::VideoCodec codec);
    bool openHardware(Discord::Voice::VideoCodec codec);
    bool openSoftware(Discord::Voice::VideoCodec codec);
    void close();
    void decode(Queued &queued);
    [[nodiscard]] bool decodeWithOpenDecoder(const Discord::Voice::EncodedVideoFrame &paddedFrame, int size);
    void startOverAtNextKeyframe();
    void reportUndecodableFrame();
    void describeDecoder();
    // TEMPORARY (Windows 7 bring-up): "[hwdiag]" lines in the log.
    void logDecodeError(const char *what, int code);
    void logSummaryNow();

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Queued> queue;
    bool queueWasDiscarded = false;
    bool stopping = false;
    std::shared_ptr<VideoFrameSink> frameSink;
    std::function<void()> keyframeRequester;
    std::function<void(Discord::Voice::VideoCodec)> codecFailureHandler;
    Stats counters;

    // Decoder thread only.
    AVCodecContext *context = nullptr;
    AVBufferRef *hardwareDevice = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *downloaded = nullptr;
    AVPacket *packet = nullptr;
    Discord::Voice::VideoCodec openCodec = Discord::Voice::VideoCodec::Unknown;
    int hardwarePixelFormat = -1;
    const char *hardwareName = nullptr;
    bool hardwareAllowed;
    int softwareThreads;
    bool hardwareRuledOut = false;
    quint64 framesSinceOpen = 0;
    bool awaitingKeyframe = true;
    double averageReadBackMs = 0;
    int decodeErrorsLogged = 0;
    QElapsedTimer summaryTimer;

    std::thread thread;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
