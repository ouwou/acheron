#include "Discord/RateLimiter.hpp"

#include <QTest>

using namespace Acheron::Discord;
using Limiter = RateLimiter<int>;
using namespace std::chrono_literals;

static RateLimitResponse status(int code, std::optional<int> retryAfterMs = std::nullopt)
{
    RateLimitResponse response;
    response.status = code;
    response.retryAfterMs = retryAfterMs;
    return response;
}

class TestRateLimiter : public QObject
{
    Q_OBJECT
private slots:
    void sendsWhenNothingIsHeld()
    {
        Limiter limiter;
        QCOMPARE(limiter.admit("/a", 1, Limiter::Clock::now()), std::optional<int>(1));
        QVERIFY(!limiter.nextWake());
    }

    void holdsAUrlAfter429UntilRetryAfter()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        limiter.record("/a", status(429, 2000), start);

        QVERIFY(!limiter.admit("/a", 1, start));
        QVERIFY(!limiter.admit("/a", 2, start));
        QCOMPARE(limiter.admit("/b", 3, start), std::optional<int>(3));

        // the backoff can make it longer than retry_after, never shorter
        QVERIFY(limiter.takeReleased(start + 1999ms).empty());
        QVERIFY(*limiter.nextWake() >= start + 2000ms);
        const auto wake = *limiter.nextWake();
        QCOMPARE(limiter.takeReleased(wake), std::vector<int>{ 1 });

        // the rest go one at a time, each after the last one finished
        QVERIFY(limiter.takeReleased(wake).empty());
        limiter.record("/a", status(200), wake);
        QCOMPARE(limiter.takeReleased(wake), std::vector<int>{ 2 });
        limiter.record("/a", status(200), wake);
        QVERIFY(limiter.takeReleased(wake).empty());

        // and then the url is free again
        QCOMPARE(limiter.admit("/a", 4, wake), std::optional<int>(4));
    }

    void anotherA429ExtendsTheHold()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        limiter.record("/a", status(503), start);
        QVERIFY(!limiter.admit("/a", 1, start));
        QVERIFY(!limiter.admit("/a", 2, start));
        const auto wake = *limiter.nextWake();
        QVERIFY(wake >= start + 5000ms);
        QCOMPARE(limiter.takeReleased(wake), std::vector<int>{ 1 });

        limiter.record("/a", status(429, 1000), wake);
        QVERIFY(limiter.takeReleased(wake).empty());
        QVERIFY(*limiter.nextWake() > wake);
        QCOMPARE(limiter.takeReleased(*limiter.nextWake()), std::vector<int>{ 2 });
    }

    void exhaustedBucketHoldsUntilReset()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        RateLimitResponse response = status(200);
        response.bucketRemaining = 0;
        response.bucketResetAfterMs = 1500;
        limiter.record("/a", response, start);

        QVERIFY(!limiter.admit("/a", 1, start));
        QCOMPARE(*limiter.nextWake(), start + 1500ms);
        QCOMPARE(limiter.takeReleased(start + 1500ms), std::vector<int>{ 1 });
    }

    void globalLimitHoldsEverything()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        RateLimitResponse response = status(429, 3000);
        response.global = true;
        limiter.record("/a", response, start);

        QVERIFY(!limiter.admit("/b", 1, start));
        QVERIFY(limiter.takeReleased(start + 2999ms).empty());
        QCOMPARE(limiter.takeReleased(start + 3000ms), std::vector<int>{ 1 });
    }

    void cloudflareBlockHoldsEverythingForAMinute()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        RateLimitResponse response = status(403);
        response.blockedByCloudflare = true;
        limiter.record("/a", response, start);

        QVERIFY(!limiter.admit("/b", 1, start));
        QVERIFY(limiter.takeReleased(start + 59s).empty());
        QCOMPARE(limiter.takeReleased(start + 60s), std::vector<int>{ 1 });
    }

    void stopsBeforeCloudflareWould()
    {
        Limiter limiter;
        const auto start = Limiter::Clock::now();
        for (int i = 0; i < Limiter::maxRefusals - 1; i++)
            limiter.record("/a" + std::to_string(i), status(401), start);
        QVERIFY(!limiter.refusing(start));
        limiter.record("/b", status(403), start);
        QVERIFY(limiter.refusing(start));
        QVERIFY(!limiter.refusing(start + Limiter::refusalWindow + 1ms));
    }
};

QTEST_GUILESS_MAIN(TestRateLimiter)
#include "tst_RateLimiter.moc"
