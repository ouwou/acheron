#include "Discord/MessageQueue.hpp"

#include <QTest>

using namespace Acheron::Discord;

static HttpResponse ok()
{
    HttpResponse response;
    response.statusCode = 200;
    response.success = true;
    return response;
}

static HttpResponse rateLimited(std::optional<int> retryAfterMs, const QByteArray &body = "{}")
{
    HttpResponse response;
    response.statusCode = 429;
    response.retryAfterMs = retryAfterMs;
    response.body = body;
    return response;
}

class TestMessageQueue : public QObject
{
    Q_OBJECT

    // what the queue has sent, with the callback to answer each
    QList<QPair<QString, HttpCallback>> sent;

    MessageQueue::Send sender(const QString &id)
    {
        return [this, id](HttpCallback done) { sent.append({ id, done }); };
    }

private slots:
    void init() { sent.clear(); }

    void sendsOneAtATime()
    {
        MessageQueue queue;
        QStringList finished;
        queue.enqueue("a", sender("a"), [&](const HttpResponse &) { finished << "a"; });
        queue.enqueue("b", sender("b"), [&](const HttpResponse &) { finished << "b"; });
        QCOMPARE(sent.size(), 1);

        sent.takeFirst().second(ok());
        QCOMPARE(finished, QStringList{ "a" });
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent.first().first, QString("b"));
        sent.takeFirst().second(ok());
        QCOMPARE(finished, (QStringList{ "a", "b" }));
    }

    void retriesTheSameRequestAfterRetryAfter()
    {
        MessageQueue queue;
        int finished = 0;
        queue.enqueue("a", sender("a"), [&](const HttpResponse &response) {
            finished++;
            QVERIFY(response.success);
        });
        queue.enqueue("b", sender("b"), {});

        sent.takeFirst().second(rateLimited(50));
        QCOMPARE(finished, 0);
        QVERIFY(sent.isEmpty());

        // "a" again, ahead of "b"
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent.first().first, QString("a"));
        sent.takeFirst().second(ok());
        QCOMPARE(finished, 1);
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent.first().first, QString("b"));
    }

    void slowmodeAndMissingRetryAfterFail()
    {
        MessageQueue queue;
        QList<int> statuses;
        queue.enqueue("a", sender("a"), [&](const HttpResponse &response) { statuses << response.statusCode; });
        queue.enqueue("b", sender("b"), [&](const HttpResponse &response) { statuses << response.statusCode; });

        sent.takeFirst().second(rateLimited(50, R"({"code":20016})"));
        QTRY_COMPARE(sent.size(), 1);
        sent.takeFirst().second(rateLimited(std::nullopt));
        QCOMPARE(statuses, (QList<int>{ 429, 429 }));
    }

    void cancelsQueuedAndWaitingSends()
    {
        MessageQueue queue;
        queue.enqueue("a", sender("a"), {});
        queue.enqueue("b", sender("b"), {});
        queue.enqueue("c", sender("c"), {});

        QVERIFY(!queue.cancel("a"));
        QVERIFY(queue.cancel("b"));

        sent.takeFirst().second(rateLimited(60000));
        QVERIFY(queue.cancel("a"));
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent.first().first, QString("c"));
    }
};

QTEST_GUILESS_MAIN(TestMessageQueue)
#include "tst_MessageQueue.moc"
