#pragma once

#include <QList>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Discord/Voice/VideoFrame.hpp"

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace Acheron {
namespace Core {
namespace Media {

[[nodiscard]] bool hardwareDecodingEnabled();
void setHardwareDecodingEnabled(bool enabled);

[[nodiscard]] QList<Discord::Voice::VideoCodec> decodableVideoCodecs();

// present() runs on the decoder's thread and must take its own reference to keep the frame
class VideoFrameSink
{
public:
    virtual ~VideoFrameSink() = default;
    virtual void present(const AVFrame *frame) = 0;
    // on the decoder's thread. a sink that keeps such a frame holds one of the decoder's surfaces, of which
    // EXTRA_HARDWARE_FRAMES are set aside
    [[nodiscard]] virtual bool acceptsHardwareFrames(int format) const
    {
        Q_UNUSED(format);
        return false;
    }
};

// frames may be pushed from any thread
class VideoDecoder
{
public:
    struct Stats
    {
        QString decoderName;
        int width = 0;
        int height = 0;
        quint64 decoded = 0;
        quint64 dropped = 0;
        quint64 errors = 0;
        quint64 queuesDiscarded = 0;
        int backlogMs = 0;
        double averageDecodeMs = 0;
        bool hardware = false;
    };

    explicit VideoDecoder(bool allowHardware = hardwareDecodingEnabled(), int softwareThreads = MAX_SOFTWARE_THREADS);
    ~VideoDecoder();

    void setSink(std::shared_ptr<VideoFrameSink> sink);
    // on the decoder's thread
    void setKeyframeRequester(std::function<void()> requester);
    // on the decoder's thread
    void setCodecFailureHandler(std::function<void(Discord::Voice::VideoCodec)> handler);

    void push(Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame);

    [[nodiscard]] Stats stats() const;

    // measured on 1080p60: a decoder with time to spare catches up within moments and loses nothing,
    // while starting over costs a key frame
    static constexpr int MAX_QUEUE_AGE_MS = 1500;
    static constexpr int MAX_QUEUED_FRAMES = 90;
    static constexpr int MAX_SOFTWARE_THREADS = 4;
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

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Queued> queue;
    bool queueWasDiscarded = false;
    bool stopping = false;
    std::shared_ptr<VideoFrameSink> frameSink;
    std::function<void()> keyframeRequester;
    std::function<void(Discord::Voice::VideoCodec)> codecFailureHandler;
    Stats counters;

    // decoder thread only
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
    std::optional<std::chrono::steady_clock::time_point> hardwareRuledOutUntil;
    bool hardwareProven = false;
    std::optional<Discord::Voice::VideoCodec> unopenableCodec;
    quint64 framesSinceOpen = 0;
    bool awaitingKeyframe = true;

    std::thread thread;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
