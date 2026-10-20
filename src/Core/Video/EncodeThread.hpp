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

#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"

namespace Acheron {
namespace Core {
namespace Video {

// The parts of an encode pipeline's own thread that the screen's and the camera's share.

// The bitrate to open an encoder at, of one asked for (none when it is not above zero) and the
// most allowed (none when it is not above zero).
[[nodiscard]] int usableBitrate(int wanted, int most);

// Small stills of what is encoded, each in an image of its own, no larger than `bound`.
// Pictures on the graphics card are brought into memory for it.
class PreviewMaker
{
public:
    explicit PreviewMaker(const QSize &bound) : bound(bound) {}

    // Null when the picture could not be had.
    [[nodiscard]] QImage make(const AVFrame *picture);

private:
    QSize bound;
    Media::RgbScaler scaler;
    QImage image;
    Media::FramePointer downloaded{ av_frame_alloc() };
};

// 90 kHz timestamps from when pictures were captured. They run on from one capture to the
// next, so that a restarted one never goes back.
class RtpClock
{
public:
    [[nodiscard]] int64_t timestampOf(std::chrono::steady_clock::time_point capturedAt);

private:
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t last = -1;
};

// Runs a capture on a thread of its own with each start's settings, until it is started again
// or stopped. `capture` is given the settings and the session they belong to, and returns once
// isCurrent() says the session is over or it has given up.
template <typename Settings>
class CaptureThread
{
public:
    using Capture = std::function<void(const Settings &settings, quint64 session)>;
    // Runs under the lock change() takes, as a start is taken up.
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

    // `change` is given the settings of a start that has not been taken up yet, if there is
    // one, under the lock that a capture also takes for what it reads of its owner's requests.
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
    // Moves on with every start and stop; a capture ends when it is no longer its own.
    std::atomic<quint64> generation{ 0 };
    std::thread thread;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
