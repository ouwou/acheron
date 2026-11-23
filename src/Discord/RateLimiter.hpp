#pragma once

#include "Backoff.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Acheron {
namespace Discord {

struct RateLimitResponse
{
    int status = 0;
    std::optional<int> retryAfterMs;
    bool global = false;
    // a 403/429 page from cloudflare instead of discord's json
    bool blockedByCloudflare = false;
    std::optional<int> bucketRemaining;
    std::optional<int> bucketResetAfterMs;
};

// the official client's per-url queue after a 429/503, plus holds it doesn't have:
// global and cloudflare limits, exhausted buckets, and a stop before cloudflare bans the ip
template<typename Request>
class RateLimiter
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr int defaultRetryAfterMs = 5000;
    static constexpr auto cloudflareHold = std::chrono::seconds(60);
    // cloudflare bans an ip at 10000 in 10 minutes
    static constexpr int maxRefusals = 1000;
    static constexpr auto refusalWindow = std::chrono::minutes(10);

    // the request back when it may go now
    std::optional<Request> admit(const std::string &key, Request request, Clock::time_point now)
    {
        if (now < globalUntil) {
            globalQueue.emplace_back(key, std::move(request));
            return std::nullopt;
        }
        auto it = holds.find(key);
        if (it == holds.end())
            return request;
        it->second.queue.push_back(std::move(request));
        return std::nullopt;
    }

    void record(const std::string &key, const RateLimitResponse &response, Clock::time_point now)
    {
        if (response.status == 401 || response.status == 403 || response.status == 429)
            refusals.push_back(now);

        if (response.global || response.blockedByCloudflare) {
            auto hold = std::chrono::milliseconds(response.retryAfterMs.value_or(0));
            const Clock::time_point until = now + (response.blockedByCloudflare ? std::max<Clock::duration>(hold, cloudflareHold) : Clock::duration(hold));
            globalUntil = std::max(globalUntil, until);
        }

        if (response.status == 429 || response.status == 503) {
            Hold &hold = holds.try_emplace(key).first->second;
            const Clock::time_point until = now + std::chrono::milliseconds(hold.backoff.fail(response.retryAfterMs.value_or(defaultRetryAfterMs)));
            if (hold.retryAt < until) {
                hold.retryAt = until;
                hold.expired = false;
            }
            return;
        }

        if (response.bucketRemaining == 0 && response.bucketResetAfterMs) {
            Hold &hold = holds.try_emplace(key).first->second;
            const Clock::time_point until = now + std::chrono::milliseconds(*response.bucketResetAfterMs);
            if (hold.retryAt < until) {
                hold.retryAt = until;
                hold.expired = false;
            }
            return;
        }

        auto it = holds.find(key);
        if (it != holds.end() && it->second.expired)
            releaseNext(it);
    }

    // requests whose hold is over, to send now
    std::vector<Request> takeReleased(Clock::time_point now)
    {
        if (now >= globalUntil && !globalQueue.empty()) {
            std::deque<std::pair<std::string, Request>> waiting;
            waiting.swap(globalQueue);
            for (auto &[key, request] : waiting)
                if (auto admitted = admit(key, std::move(request), now))
                    released.push_back(std::move(*admitted));
        }

        for (auto it = holds.begin(); it != holds.end();) {
            auto next = std::next(it);
            if (!it->second.expired && now >= it->second.retryAt) {
                it->second.expired = true;
                releaseNext(it);
            }
            it = next;
        }

        std::vector<Request> out;
        out.swap(released);
        return out;
    }

    [[nodiscard]] std::optional<Clock::time_point> nextWake() const
    {
        std::optional<Clock::time_point> wake;
        if (!globalQueue.empty())
            wake = globalUntil;
        for (const auto &[key, hold] : holds)
            if (!hold.expired && (!wake || hold.retryAt < *wake))
                wake = hold.retryAt;
        return wake;
    }

    [[nodiscard]] bool refusing(Clock::time_point now)
    {
        while (!refusals.empty() && now - refusals.front() > refusalWindow)
            refusals.pop_front();
        return refusals.size() >= maxRefusals;
    }

    void clear()
    {
        holds.clear();
        globalQueue.clear();
        released.clear();
    }

private:
    struct Hold
    {
        std::deque<Request> queue;
        Clock::time_point retryAt;
        Backoff backoff{ 1000, 60000 };
        // after this, each finished request lets the next one go
        bool expired = false;
    };

    void releaseNext(typename std::map<std::string, Hold>::iterator it)
    {
        if (it->second.queue.empty()) {
            holds.erase(it);
            return;
        }
        released.push_back(std::move(it->second.queue.front()));
        it->second.queue.pop_front();
    }

    std::map<std::string, Hold> holds;
    std::deque<std::pair<std::string, Request>> globalQueue;
    Clock::time_point globalUntil;
    std::vector<Request> released;
    std::deque<Clock::time_point> refusals;
};

} // namespace Discord
} // namespace Acheron
