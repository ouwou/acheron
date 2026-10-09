#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <functional>
#include <optional>

#include "Discord/Entities.hpp"
#include "Result.hpp"
#include "Snowflake.hpp"

namespace Acheron {

namespace Discord {
class Client;
}

namespace Core {

class UserManager;

struct ReactionRef
{
    Snowflake messageId;
    Discord::Emoji emoji;
    bool isBurst = false;

    [[nodiscard]] bool sameReaction(const ReactionRef &other) const;
};

class MessageReactors : public QObject
{
    Q_OBJECT
public:
    static constexpr int TooltipSample = 3;
    static constexpr int PageSize = 100;

    MessageReactors(Discord::Client *client, UserManager *users, QObject *parent = nullptr);

    [[nodiscard]] QList<Discord::User> reactors(Snowflake channelId, const ReactionRef &reaction, int firstReadLimit);

    using PageFetched = std::function<void(const Result<QList<Snowflake>> &pageReactorIds)>;
    void fetchPage(Snowflake channelId, const ReactionRef &reaction, std::optional<Snowflake> after, PageFetched onFetched);

    void add(const ReactionRef &reaction, Snowflake userId);
    void remove(const ReactionRef &reaction, Snowflake userId);
    void forgetReactors(Snowflake messageId);
    void forgetReactors(Snowflake messageId, const Discord::Emoji &emoji);

signals:
    void reactorsChanged(Core::Snowflake messageId);

private slots:
    void onReady();

private:
    struct Key
    {
        Snowflake messageId;
        Snowflake customEmojiId;
        QString unicodeEmoji;
        bool isBurst;

        bool operator==(const Key &other) const
        {
            return messageId == other.messageId && customEmojiId == other.customEmojiId && unicodeEmoji == other.unicodeEmoji && isBurst == other.isBurst;
        }

        friend size_t qHash(const Key &key, size_t seed = 0)
        {
            return qHashMulti(seed, key.messageId, key.customEmojiId, key.unicodeEmoji, key.isBurst);
        }
    };

    struct Known
    {
        bool askedApi = false;
        QList<Snowflake> reactorIds;
    };

    [[nodiscard]] static Key keyOf(const ReactionRef &reaction);
    void fetch(Snowflake channelId, const ReactionRef &reaction, int limit, std::optional<Snowflake> after, PageFetched onFetched);

    Discord::Client *client;
    UserManager *users;
    QHash<Key, Known> knownByReaction;
};

} // namespace Core
} // namespace Acheron
