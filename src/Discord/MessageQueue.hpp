#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <deque>
#include <functional>
#include <optional>

#include "HttpClient.hpp"

namespace Acheron {
namespace Discord {

// the official client's MessageQueue: sends and edits go out one at a time, and a 429
// puts the same request (same nonce) back at the front until its Retry-After passes
class MessageQueue : public QObject
{
    Q_OBJECT
public:
    using Send = std::function<void(HttpCallback)>;

    explicit MessageQueue(QObject *parent = nullptr);

    // id is the nonce of a send or the id of the edited message
    void enqueue(const QString &id, Send send, HttpCallback done);
    // false once it has been sent
    bool cancel(const QString &id);

    [[nodiscard]] int size() const { return static_cast<int>(queue.size()); }

private:
    struct Item
    {
        QString id;
        Send send;
        HttpCallback done;
    };

    void drain();
    void finish(Item item, const HttpResponse &response);

    std::deque<Item> queue;
    std::optional<Item> waitingToRetry;
    QTimer retryTimer;
    bool draining = false;
};

} // namespace Discord
} // namespace Acheron
