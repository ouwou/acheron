#include "MessageQueue.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace Acheron {
namespace Discord {

static bool isSlowmode(const HttpResponse &response)
{
    constexpr int slowmodeRateLimited = 20016;
    constexpr int channelFollowingEditRateLimited = 20022;
    const int code = QJsonDocument::fromJson(response.body).object().value("code").toInt();
    return code == slowmodeRateLimited || code == channelFollowingEditRateLimited;
}

MessageQueue::MessageQueue(QObject *parent) : QObject(parent)
{
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, [this] {
        queue.push_front(std::move(*waitingToRetry));
        waitingToRetry.reset();
        drain();
    });
}

void MessageQueue::enqueue(const QString &id, Send send, HttpCallback done)
{
    queue.push_back({ id, std::move(send), std::move(done) });
    drain();
}

bool MessageQueue::cancel(const QString &id)
{
    const auto before = queue.size();
    queue.erase(std::remove_if(queue.begin(), queue.end(), [&id](const Item &item) { return item.id == id; }), queue.end());
    bool removed = queue.size() != before;

    if (waitingToRetry && waitingToRetry->id == id) {
        retryTimer.stop();
        waitingToRetry.reset();
        removed = true;
        drain();
    }
    return removed;
}

void MessageQueue::drain()
{
    if (draining || waitingToRetry || queue.empty())
        return;

    draining = true;
    Item item = std::move(queue.front());
    queue.pop_front();
    Send send = item.send;
    send([this, item = std::move(item)](const HttpResponse &response) mutable {
        finish(std::move(item), response);
    });
}

void MessageQueue::finish(Item item, const HttpResponse &response)
{
    draining = false;

    if (response.rateLimited() && response.retryAfterMs && !isSlowmode(response)) {
        retryTimer.start(*response.retryAfterMs);
        waitingToRetry = std::move(item);
        return;
    }

    QTimer::singleShot(0, this, &MessageQueue::drain);
    if (item.done)
        item.done(response);
}

} // namespace Discord
} // namespace Acheron
