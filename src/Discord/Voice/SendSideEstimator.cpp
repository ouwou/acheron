#include "SendSideEstimator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

// bitrate_estimator.cc
constexpr int INITIAL_WINDOW_MS = 500;
constexpr int WINDOW_MS = 150;
constexpr float UNCERTAINTY_SCALE = 10;
constexpr float VARIANCE_GROWTH = 5;

// inter_arrival_delta.cc
constexpr qint64 GROUP_LENGTH_US = 5000;
constexpr qint64 BURST_DELTA_US = 5000;
constexpr qint64 MAX_BURST_US = 100'000;
constexpr qint64 ARRIVAL_OFFSET_LIMIT_US = 3'000'000;
constexpr int REORDERED_BEFORE_RESET = 3;

// trendline_estimator.cc
constexpr size_t TREND_WINDOW = 20;
constexpr double SMOOTHING = 0.9;
constexpr double THRESHOLD_GAIN = 4.0;
constexpr double THRESHOLD_UP = 0.0087;
constexpr double THRESHOLD_DOWN = 0.039;
constexpr double OVERUSING_BEFORE_SAID_MS = 10;
constexpr double MAX_ADAPT_OFFSET_MS = 15;
constexpr double MAX_ADAPT_STEP_MS = 100;
constexpr double MIN_THRESHOLD = 6;
constexpr double MAX_THRESHOLD = 600;
constexpr int DELTAS_COUNTED = 60;
constexpr int MAX_DELTAS = 1000;

// aimd_rate_control.cc
constexpr double BACK_OFF = 0.85;
constexpr double MIN_BITRATE = 5000;
constexpr double INCREASE_PER_SECOND = 1.08;
constexpr double MIN_MULTIPLICATIVE_STEP = 1000;
constexpr double THROUGHPUT_HEADROOM = 1.5;
constexpr double THROUGHPUT_HEADROOM_BITS = 10000;
constexpr double MIN_ADDITIVE_PER_SECOND = 4000;
constexpr qint64 MIN_REDUCTION_INTERVAL_US = 10'000;
constexpr qint64 MAX_REDUCTION_INTERVAL_US = 200'000;
constexpr qint64 DETECTOR_DELAY_US = 100'000;
constexpr double FRAMES_PER_SECOND = 30;
constexpr double PACKET_BYTES = 1200;

// link_capacity_estimator.cc
constexpr double OVERUSE_WEIGHT = 0.05;
constexpr double MIN_DEVIATION = 0.4;
constexpr double MAX_DEVIATION = 2.5;

struct Arrived
{
    qint64 sendUs = 0;
    qint64 arrivalUs = 0;
    int bytes = 0;
};

} // namespace

void SendSideEstimator::restart(uint16_t firstSequence)
{
    *this = SendSideEstimator();
    firstHeld = firstSequence;
    nextNumber = firstSequence;
}

uint16_t SendSideEstimator::numberPacket(qint64 nowUs, int bytes)
{
    while (!sent.empty() && (static_cast<int>(sent.size()) >= MAX_PACKETS_HELD || nowUs - sent.front().sentUs > HELD_FOR_US)) {
        sent.pop_front();
        firstHeld++;
    }
    sent.push_back({ nowUs, bytes });
    return static_cast<uint16_t>(nextNumber++);
}

void SendSideEstimator::setRoundTrip(qint64 roundTripUs)
{
    rate.roundTripUs = roundTripUs;
}

void SendSideEstimator::forgetDelays()
{
    grouping = Grouping();
    trend = Trend();
}

bool SendSideEstimator::onReport(const ReceivedRtcp::TransportReport &report, qint64 nowUs)
{
    if (sent.empty())
        return false;
    reports++;

    if (haveReference) {
        const int32_t moved = signExtend24(static_cast<uint32_t>(report.referenceTime - lastReference));
        referenceOffsetUs += moved * REFERENCE_UNIT_US;
    } else {
        referenceOffsetUs = nowUs;
        haveReference = true;
    }
    lastReference = report.referenceTime;

    const qint64 newest = nextNumber - 1;
    const qint64 base = newest - static_cast<uint16_t>(static_cast<uint16_t>(newest) - report.baseSequence);

    std::vector<Arrived> arrived;
    qint64 arrivalUs = referenceOffsetUs;
    for (size_t i = 0; i < report.packets.size(); i++) {
        if (!report.packets[i].received)
            continue;
        arrivalUs += report.packets[i].delta * DELTA_UNIT_US;

        const qint64 number = base + static_cast<qint64>(i);
        if (number < firstHeld || number >= nextNumber)
            continue;
        Sent &packet = sent[static_cast<size_t>(number - firstHeld)];
        if (packet.bytes == 0)
            continue;
        arrived.push_back({ packet.sentUs, arrivalUs, packet.bytes });
        packet.bytes = 0;
    }
    if (arrived.empty())
        return false;
    std::stable_sort(arrived.begin(), arrived.end(), [](const Arrived &a, const Arrived &b) { return a.arrivalUs < b.arrivalUs; });

    if (lastArrivalSeenUs >= 0 && nowUs - lastArrivalSeenUs > SILENCE_BEFORE_RESTART_US)
        forgetDelays();
    lastArrivalSeenUs = nowUs;

    for (const Arrived &packet : arrived) {
        throughput.add(packet.arrivalUs / 1000, packet.bytes);

        if (const std::optional<Grouping::Deltas> deltas = grouping.add(packet.sendUs, packet.arrivalUs, nowUs))
            trend.add(deltas->arrivalUs / 1000.0, deltas->sendUs / 1000.0, packet.arrivalUs / 1000.0);
    }

    std::optional<double> acknowledgedNow;
    if (throughput.estimateKbps)
        acknowledgedNow = *throughput.estimateKbps * 1000.0;
    if (trend.usage == Usage::Overusing && !(acknowledgedNow && rate.timeToReduceFurther(nowUs, *acknowledgedNow)))
        return false;
    return rate.update(trend.usage, acknowledgedNow, nowUs);
}

void SendSideEstimator::Throughput::add(qint64 atMs, int bytes)
{
    const std::optional<float> sampleKbps = closeWindow(atMs, bytes, estimateKbps ? WINDOW_MS : INITIAL_WINDOW_MS);
    if (!sampleKbps)
        return;
    if (!estimateKbps) {
        estimateKbps = sampleKbps;
        return;
    }

    const float uncertainty = UNCERTAINTY_SCALE * std::abs(*estimateKbps - *sampleKbps) / std::max(*estimateKbps, 1e-3f);
    const float sampleVariance = uncertainty * uncertainty;
    const float predictedVariance = variance + VARIANCE_GROWTH;
    estimateKbps = (sampleVariance * *estimateKbps + predictedVariance * *sampleKbps) / (sampleVariance + predictedVariance);
    variance = sampleVariance * predictedVariance / (sampleVariance + predictedVariance);
}

std::optional<float> SendSideEstimator::Throughput::closeWindow(qint64 atMs, int bytes, int windowLengthMs)
{
    if (atMs < previousMs) {
        previousMs = -1;
        sum = 0;
        windowMs = 0;
    }
    if (previousMs >= 0) {
        windowMs += atMs - previousMs;
        if (atMs - previousMs > windowLengthMs) {
            sum = 0;
            windowMs %= windowLengthMs;
        }
    }
    previousMs = atMs;

    std::optional<float> sampleKbps;
    if (windowMs >= windowLengthMs) {
        sampleKbps = 8.0f * static_cast<float>(sum) / static_cast<float>(windowLengthMs);
        windowMs -= windowLengthMs;
        sum = 0;
    }
    sum += bytes;
    return sampleKbps;
}

std::optional<qint64> SendSideEstimator::estimate() const
{
    if (!rate.initialized)
        return std::nullopt;
    return static_cast<qint64>(rate.current);
}

std::optional<qint64> SendSideEstimator::acknowledged() const
{
    if (!throughput.estimateKbps)
        return std::nullopt;
    return static_cast<qint64>(*throughput.estimateKbps * 1000);
}

bool SendSideEstimator::Grouping::belongsToBurst(qint64 arrivalUs, qint64 sendUs) const
{
    const qint64 arrivalDeltaUs = arrivalUs - current.completeUs;
    const qint64 sendDeltaUs = sendUs - current.sendUs;
    if (sendDeltaUs == 0)
        return true;
    return arrivalDeltaUs - sendDeltaUs < 0 && arrivalDeltaUs <= BURST_DELTA_US && arrivalUs - current.firstArrivalUs < MAX_BURST_US;
}

std::optional<SendSideEstimator::Grouping::Deltas> SendSideEstimator::Grouping::add(qint64 sendUs, qint64 arrivalUs, qint64 systemUs)
{
    std::optional<Deltas> completed;
    if (current.isEmpty()) {
        current.sendUs = sendUs;
        current.firstSendUs = sendUs;
        current.firstArrivalUs = arrivalUs;
    } else if (current.firstSendUs > sendUs) {
        return std::nullopt;
    } else if (!belongsToBurst(arrivalUs, sendUs) && sendUs - current.firstSendUs > GROUP_LENGTH_US) {
        if (!previous.isEmpty()) {
            const Deltas deltas{ current.sendUs - previous.sendUs, current.completeUs - previous.completeUs };
            const qint64 systemDeltaUs = current.lastSystemUs - previous.lastSystemUs;
            if (deltas.arrivalUs - systemDeltaUs >= ARRIVAL_OFFSET_LIMIT_US) {
                *this = Grouping();
                return std::nullopt;
            }
            if (deltas.arrivalUs < 0) {
                if (++reorderedInARow >= REORDERED_BEFORE_RESET)
                    *this = Grouping();
                return std::nullopt;
            }
            reorderedInARow = 0;
            completed = deltas;
        }
        previous = current;
        current.firstSendUs = sendUs;
        current.sendUs = sendUs;
        current.firstArrivalUs = arrivalUs;
    } else {
        current.sendUs = std::max(current.sendUs, sendUs);
    }
    current.completeUs = arrivalUs;
    current.lastSystemUs = systemUs;
    return completed;
}

void SendSideEstimator::Trend::add(double arrivalDeltaMs, double sendDeltaMs, double arrivalMs)
{
    deltas = std::min(deltas + 1, MAX_DELTAS);
    if (firstArrivalMs < 0)
        firstArrivalMs = arrivalMs;

    accumulatedDelayMs += arrivalDeltaMs - sendDeltaMs;
    smoothedDelayMs = SMOOTHING * smoothedDelayMs + (1 - SMOOTHING) * accumulatedDelayMs;
    window.push_back({ arrivalMs - firstArrivalMs, smoothedDelayMs });
    if (window.size() > TREND_WINDOW)
        window.pop_front();

    double slope = previousSlope;
    if (window.size() == TREND_WINDOW) {
        double meanArrival = 0;
        double meanDelay = 0;
        for (const Point &point : window) {
            meanArrival += point.arrivalMs;
            meanDelay += point.smoothedDelayMs;
        }
        meanArrival /= static_cast<double>(window.size());
        meanDelay /= static_cast<double>(window.size());

        double numerator = 0;
        double denominator = 0;
        for (const Point &point : window) {
            numerator += (point.arrivalMs - meanArrival) * (point.smoothedDelayMs - meanDelay);
            denominator += (point.arrivalMs - meanArrival) * (point.arrivalMs - meanArrival);
        }
        if (denominator != 0)
            slope = numerator / denominator;
    }
    detect(slope, sendDeltaMs, arrivalMs);
}

void SendSideEstimator::Trend::detect(double slope, double sendDeltaMs, double nowMs)
{
    if (deltas < 2) {
        usage = Usage::Normal;
        return;
    }

    const double modifiedTrend = std::min(deltas, DELTAS_COUNTED) * slope * THRESHOLD_GAIN;
    if (modifiedTrend > threshold) {
        overusingForMs = overusingForMs < 0 ? sendDeltaMs / 2 : overusingForMs + sendDeltaMs;
        overuses++;
        if (overusingForMs > OVERUSING_BEFORE_SAID_MS && overuses > 1 && slope >= previousSlope) {
            overusingForMs = 0;
            overuses = 0;
            usage = Usage::Overusing;
        }
    } else {
        overusingForMs = -1;
        overuses = 0;
        usage = modifiedTrend < -threshold ? Usage::Underusing : Usage::Normal;
    }
    previousSlope = slope;
    adaptThreshold(modifiedTrend, nowMs);
}

void SendSideEstimator::Trend::adaptThreshold(double modifiedTrend, double nowMs)
{
    if (thresholdAdaptedMs < 0)
        thresholdAdaptedMs = nowMs;

    if (std::abs(modifiedTrend) > threshold + MAX_ADAPT_OFFSET_MS) {
        thresholdAdaptedMs = nowMs;
        return;
    }

    const double gain = std::abs(modifiedTrend) < threshold ? THRESHOLD_DOWN : THRESHOLD_UP;
    const double elapsedMs = std::min(nowMs - thresholdAdaptedMs, MAX_ADAPT_STEP_MS);
    threshold = std::clamp(threshold + gain * (std::abs(modifiedTrend) - threshold) * elapsedMs, MIN_THRESHOLD, MAX_THRESHOLD);
    thresholdAdaptedMs = nowMs;
}

double SendSideEstimator::Capacity::deviationKbps() const
{
    return std::sqrt(deviation * *estimateKbps);
}

double SendSideEstimator::Capacity::upperBound() const
{
    return estimateKbps ? (*estimateKbps + 3 * deviationKbps()) * 1000.0 : std::numeric_limits<double>::infinity();
}

double SendSideEstimator::Capacity::lowerBound() const
{
    return estimateKbps ? std::max(0.0, *estimateKbps - 3 * deviationKbps()) * 1000.0 : 0.0;
}

void SendSideEstimator::Capacity::onOveruse(double bitsPerSecond)
{
    const double sampleKbps = bitsPerSecond / 1000.0;
    estimateKbps = estimateKbps ? (1 - OVERUSE_WEIGHT) * *estimateKbps + OVERUSE_WEIGHT * sampleKbps : sampleKbps;

    const double error = *estimateKbps - sampleKbps;
    deviation = std::clamp((1 - OVERUSE_WEIGHT) * deviation + OVERUSE_WEIGHT * error * error / std::max(*estimateKbps, 1.0), MIN_DEVIATION, MAX_DEVIATION);
}

bool SendSideEstimator::Rate::timeToReduceFurther(qint64 nowUs, double acknowledged) const
{
    if (changedUs < 0 || nowUs - changedUs >= std::clamp(roundTripUs, MIN_REDUCTION_INTERVAL_US, MAX_REDUCTION_INTERVAL_US))
        return true;
    return initialized && acknowledged < 0.5 * current;
}

double SendSideEstimator::Rate::nearMaxIncreasePerSecond() const
{
    const double frameBits = current / FRAMES_PER_SECOND;
    const double packetsPerFrame = std::ceil(frameBits / 8 / PACKET_BYTES);
    const double packetBits = frameBits / std::max(packetsPerFrame, 1.0);
    const double responseSeconds = static_cast<double>(roundTripUs + DETECTOR_DELAY_US) * 2 / 1e6;
    return std::max(MIN_ADDITIVE_PER_SECOND, packetBits / responseSeconds);
}

bool SendSideEstimator::Rate::update(Usage usage, std::optional<double> acknowledged, qint64 nowUs)
{
    const double before = current;
    const bool wasInitialized = initialized;
    const double throughputNow = acknowledged.value_or(latestAcknowledged);
    if (acknowledged)
        latestAcknowledged = *acknowledged;

    if (!initialized && usage != Usage::Overusing)
        return false;

    if (usage == Usage::Overusing) {
        state = State::Decrease;
    } else if (usage == Usage::Underusing) {
        state = State::Hold;
    } else if (state == State::Hold) {
        changedUs = nowUs;
        state = State::Increase;
    }

    std::optional<double> wanted;
    if (state == State::Increase) {
        if (throughputNow > capacity.upperBound())
            capacity.estimateKbps.reset();

        const double limit = THROUGHPUT_HEADROOM * throughputNow + THROUGHPUT_HEADROOM_BITS;
        if (current < limit) {
            const double seconds = changedUs < 0 ? 0.0 : static_cast<double>(nowUs - changedUs) / 1e6;
            const double step = capacity.estimateKbps ? nearMaxIncreasePerSecond() * seconds : std::max(current * (std::pow(INCREASE_PER_SECOND, changedUs < 0 ? 1.0 : std::min(seconds, 1.0)) - 1), MIN_MULTIPLICATIVE_STEP);
            wanted = std::min(current + step, limit);
        }
        changedUs = nowUs;
    } else if (state == State::Decrease) {
        double decreased = throughputNow * BACK_OFF;
        if (decreased > current && capacity.estimateKbps)
            decreased = BACK_OFF * *capacity.estimateKbps * 1000.0;
        if (decreased < current)
            wanted = decreased;

        if (throughputNow < capacity.lowerBound())
            capacity.estimateKbps.reset();
        capacity.onOveruse(throughputNow);
        initialized = true;
        state = State::Hold;
        changedUs = nowUs;
    }

    current = std::max(wanted.value_or(current), MIN_BITRATE);
    return current != before || initialized != wasInitialized;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
