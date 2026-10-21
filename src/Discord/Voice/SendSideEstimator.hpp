#pragma once

#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <optional>

#include "Rtcp.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// the delay-based half of WebRTC's GoogCC, which the official client runs unchanged, with its numbers.
// nothing probes the way, so no limit is known until it has been overused once
class SendSideEstimator
{
public:
    void restart(uint16_t firstSequence);

    uint16_t numberPacket(qint64 nowUs, int bytes);
    void setRoundTrip(qint64 roundTripUs);
    bool onReport(const ReceivedRtcp::TransportReport &report, qint64 nowUs);

    [[nodiscard]] std::optional<qint64> estimate() const;
    [[nodiscard]] std::optional<qint64> acknowledged() const;
    [[nodiscard]] quint64 reportsTaken() const { return reports; }
    [[nodiscard]] bool overusing() const { return trend.usage == Usage::Overusing; }

    static constexpr int MAX_PACKETS_HELD = 20000;
    static constexpr qint64 HELD_FOR_US = 60'000'000;
    static constexpr qint64 SILENCE_BEFORE_RESTART_US = 2'000'000;

private:
    enum class Usage {
        Normal,
        Underusing,
        Overusing,
    };

    struct Sent
    {
        qint64 sentUs = 0;
        int bytes = 0;
    };

    struct Throughput
    {
        void add(qint64 atMs, int bytes);
        [[nodiscard]] std::optional<float> closeWindow(qint64 atMs, int bytes, int windowMs);

        std::optional<float> estimateKbps;
        float variance = 50;
        qint64 sum = 0;
        qint64 windowMs = 0;
        qint64 previousMs = -1;
    };

    struct Grouping
    {
        struct Group
        {
            qint64 firstSendUs = -1;
            qint64 sendUs = -1;
            qint64 firstArrivalUs = -1;
            qint64 completeUs = -1;
            qint64 lastSystemUs = -1;
            [[nodiscard]] bool isEmpty() const { return completeUs < 0; }
        };

        struct Deltas
        {
            qint64 sendUs = 0;
            qint64 arrivalUs = 0;
        };

        // the deltas between the last two groups, once a packet completes one
        [[nodiscard]] std::optional<Deltas> add(qint64 sendUs, qint64 arrivalUs, qint64 systemUs);
        [[nodiscard]] bool belongsToBurst(qint64 arrivalUs, qint64 sendUs) const;

        Group current;
        Group previous;
        int reorderedInARow = 0;
    };

    struct Trend
    {
        struct Point
        {
            double arrivalMs = 0;
            double smoothedDelayMs = 0;
        };

        void add(double arrivalDeltaMs, double sendDeltaMs, double arrivalMs);
        void detect(double slope, double sendDeltaMs, double nowMs);
        void adaptThreshold(double modifiedTrend, double nowMs);

        std::deque<Point> window;
        double firstArrivalMs = -1;
        double accumulatedDelayMs = 0;
        double smoothedDelayMs = 0;
        int deltas = 0;
        double threshold = 12.5;
        double thresholdAdaptedMs = -1;
        double previousSlope = 0;
        double overusingForMs = -1;
        int overuses = 0;
        Usage usage = Usage::Normal;
    };

    struct Capacity
    {
        void onOveruse(double bitsPerSecond);
        [[nodiscard]] double upperBound() const;
        [[nodiscard]] double lowerBound() const;
        [[nodiscard]] double deviationKbps() const;

        std::optional<double> estimateKbps;
        double deviation = 0.4;
    };

    struct Rate
    {
        enum class State {
            Hold,
            Increase,
            Decrease,
        };

        [[nodiscard]] bool timeToReduceFurther(qint64 nowUs, double acknowledged) const;
        bool update(Usage usage, std::optional<double> acknowledged, qint64 nowUs);
        [[nodiscard]] double nearMaxIncreasePerSecond() const;

        State state = State::Hold;
        double current = 30'000'000;
        double latestAcknowledged = 30'000'000;
        bool initialized = false;
        qint64 changedUs = -1;
        qint64 roundTripUs = 200'000;
        Capacity capacity;
    };

    void forgetDelays();

    std::deque<Sent> sent;
    qint64 firstHeld = 0;
    qint64 nextNumber = 0;

    bool haveReference = false;
    int32_t lastReference = 0;
    qint64 referenceOffsetUs = 0;
    qint64 lastArrivalSeenUs = -1;
    quint64 reports = 0;

    Throughput throughput;
    Grouping grouping;
    Trend trend;
    Rate rate;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
