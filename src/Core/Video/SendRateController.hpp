#pragma once

#include <QtGlobal>

#include <optional>

namespace Acheron {
namespace Core {
namespace Video {

// the receiver's estimate caps it, as in the official client. every change can cost the encoder a key
// frame, so it makes few
class SendRateController
{
public:
    void restart(int minBitrate, int maxBitrate);
    bool setLimits(int minBitrate, int maxBitrate);
    bool report(std::optional<double> lossFraction, qint64 nowMs);
    bool setReceiverLimit(std::optional<qint64> bitsPerSecond);

    [[nodiscard]] int bitrate() const;

    static constexpr double HIGH_LOSS = 0.10;
    static constexpr double LOW_LOSS = 0.02;
    static constexpr double DECREASE = 0.7;
    static constexpr double INCREASE = 1.15;
    static constexpr qint64 DECREASE_INTERVAL_MS = 2000;
    static constexpr qint64 CLEAN_BEFORE_INCREASE_MS = 10000;

private:
    void setBounds(int minBitrate, int maxBitrate);
    void moveTo(double wanted);

    int floor = 0;
    int ceiling = 0;
    int current = 0;
    std::optional<qint64> receiverLimit;
    std::optional<qint64> loweredAtMs;
    std::optional<qint64> cleanSinceMs;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
