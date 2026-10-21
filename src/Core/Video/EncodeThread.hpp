#pragma once

#include <QImage>
#include <QSize>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/EncodePipeline.hpp"

namespace Acheron {
namespace Core {
namespace Video {

[[nodiscard]] int usableBitrate(std::optional<int> wanted, std::optional<int> most);

class PreviewMaker
{
public:
    explicit PreviewMaker(const QSize &bound) : bound(bound) {}

    [[nodiscard]] QImage make(const AVFrame *picture);

private:
    QSize bound;
    Media::RgbScaler scaler;
    QImage image;
    Media::FramePointer downloaded{ av_frame_alloc() };
};

class EncodeCounters
{
public:
    // a picture that came out as nothing counts as dropped
    void deliver(std::vector<Discord::Voice::EncodedVideoFrame> &encoded, EncodePipeline *pipeline, int layer);
    [[nodiscard]] PipelineStats takeStats(double seconds, const Media::VideoEncoder::Stats &encoder, const QString &encoderName, const QString &source, const QSize &encodedSize, int capturedLately);

private:
    quint64 framesEncoded = 0;
    quint64 framesDropped = 0;
    int encodedLately = 0;
    quint64 bytesLately = 0;
};

[[nodiscard]] QString encoderOrNone(const Media::VideoEncoder::Stats &encoder);

class RtpClock
{
public:
    [[nodiscard]] int64_t timestampOf(std::chrono::steady_clock::time_point capturedAt);

private:
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t last = -1;
};

// `capture` returns once isCurrent() says its session is over, or it has given up
template <typename Settings>
class CaptureThread
{
public:
    using Capture = std::function<void(const Settings &settings, quint64 session)>;
    // runs under change()'s lock
    using Taken = std::function<void(const Settings &settings)>;

    explicit CaptureThread(Capture capture, Taken taken = {})
        : capture(std::move(capture)), taken(std::move(taken)), thread([this] { run(); })
    {
    }

    ~CaptureThread()
    {
        {
            std::lock_guard lock(mutex);
            quitting = true;
            generation++;
        }
        wake.notify_all();
        thread.join();
    }

    CaptureThread(const CaptureThread &) = delete;
    CaptureThread &operator=(const CaptureThread &) = delete;

    void start(const Settings &settings)
    {
        {
            std::lock_guard lock(mutex);
            pending = settings;
            generation++;
        }
        wake.notify_all();
    }

    void stop()
    {
        std::lock_guard lock(mutex);
        pending.reset();
        generation++;
    }

    [[nodiscard]] bool isCurrent(quint64 session) const { return generation == session; }

    // under the lock a capture also takes to read its owner's requests
    template <typename Change>
    void change(Change &&change)
    {
        std::lock_guard lock(mutex);
        change(pending);
    }

private:
    void run()
    {
        for (;;) {
            Settings settings;
            quint64 session = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return quitting || pending; });
                if (quitting)
                    return;
                settings = *std::exchange(pending, std::nullopt);
                session = generation;
                if (taken)
                    taken(settings);
            }
            capture(settings, session);
        }
    }

    const Capture capture;
    const Taken taken;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Settings> pending;
    bool quitting = false;
    std::atomic<quint64> generation{ 0 };
    std::thread thread;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
