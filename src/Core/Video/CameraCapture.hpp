#pragma once

#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Core/Media/FfmpegPointers.hpp"

namespace Acheron {
namespace Core {
namespace Video {

// belongs to the thread that reads it
class CameraFrameSource
{
public:
    enum class Status {
        Frame,
        NoFrame,
        Ended,
    };

    virtual ~CameraFrameSource() = default;

    [[nodiscard]] virtual bool open(const QString &deviceId) = 0;
    // `into` must be empty. a slow reader sees fewer pictures, never older ones. `capturedAt` only grows
    [[nodiscard]] virtual Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) = 0;
    [[nodiscard]] virtual QString failure() const = 0;
    [[nodiscard]] virtual QString description() const = 0;
};

using CameraFrameSourceFactory = std::function<std::unique_ptr<CameraFrameSource>()>;

void setCameraFrameSourceFactory(CameraFrameSourceFactory factory);
[[nodiscard]] std::unique_ptr<CameraFrameSource> makeCameraFrameSource();

// a camera's own timestamps space pictures more truly than the times they were read; the shortest
// delivery delay seen ties the two clocks together
class CaptureClock
{
public:
    [[nodiscard]] std::chrono::steady_clock::time_point at(std::optional<std::chrono::microseconds> stamped, std::chrono::steady_clock::time_point readAt);

private:
    std::optional<std::chrono::steady_clock::duration> ourClockAhead;
    std::chrono::steady_clock::time_point lastReadAt;
    std::optional<std::chrono::steady_clock::time_point> last;
};

#if defined(ACHERON_HAVE_AVDEVICE) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))

class CameraCapture : public CameraFrameSource
{
public:
    CameraCapture() = default;
    ~CameraCapture() override;

    CameraCapture(const CameraCapture &) = delete;
    CameraCapture &operator=(const CameraCapture &) = delete;

    [[nodiscard]] bool open(const QString &deviceId) override;
    [[nodiscard]] Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) override;
    [[nodiscard]] QString failure() const override;
    [[nodiscard]] QString description() const override;

private:
    enum class State {
        Opening,
        Open,
        Ended,
    };

    void run(const QString &deviceId);
    void opened(const QString &mode);
    void end(const QString &reason);
    void publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt);

    mutable std::mutex mutex;
    std::condition_variable changed;
    State state = State::Opening;
    QString failureText;
    QString mode;
    const Media::FramePointer newest{ av_frame_alloc() };
    std::chrono::steady_clock::time_point newestCapturedAt;

    std::atomic<bool> stopping{ false };
    std::thread thread;
};

#endif

} // namespace Video
} // namespace Core
} // namespace Acheron
