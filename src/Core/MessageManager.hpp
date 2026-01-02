#pragma once

#include <deque>

#include "Snowflake.hpp"
#include "Storage/MessageRepository.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace Core {

struct MessageRequestResult
{
    // not using Result cuz i want to see the type and id even if it failed
    bool success;
    Discord::Client::MessageLoadType type;
    Snowflake channelId;
    QList<Discord::Message> messages;
};

class MessageManager : public QObject
{
    Q_OBJECT
public:
    explicit MessageManager(Snowflake accountId, Discord::Client *client,
                            QObject *parent = nullptr);
    ~MessageManager() override;

    void requestLoadChannel(Snowflake channelId);
    void requestLoadHistory(Snowflake channelId, Snowflake beforeId);

signals:
    void messagesReceived(const MessageRequestResult &result);

public slots:
    void onMessageCreated(const Discord::Message &message);

private slots:
    void onApiMessagesReceived(const QList<Discord::Message> &messages,
                               Discord::Client::MessageLoadType type, Snowflake channelId);

private:
    void cacheMessages(Snowflake channelId, const QList<Discord::Message> &msgs);
    QList<Discord::Message> getCachedMessages();

    Storage::MessageRepository repo;

    Discord::Client *client;

    QCache<Snowflake, Discord::Message> messageCache;
    QHash<Snowflake, std::deque<Snowflake>> channelMessages;
    QSet<Snowflake> fetchedChannels;
    QHash<Snowflake, Snowflake> lowestKnownId;
    QSet<Snowflake> historyDebounce;
};

} // namespace Core
} // namespace Acheron
