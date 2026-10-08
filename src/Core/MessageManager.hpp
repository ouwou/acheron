#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <QCache>
#include <QPair>
#include <QSet>

#include "MessageSegments.hpp"
#include "PendingAttachment.hpp"
#include "Snowflake.hpp"
#include "Storage/MessageRepository.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Discord/Client.hpp"
#include "Markdown/Parser.hpp"

namespace Acheron {
namespace Core {

class UserManager;
class EmojiManager;

struct MessageRequestResult
{
    // not using Result cuz i want to see the type and id even if it failed
    bool success;
    Discord::Client::MessageLoadType type;
    Snowflake channelId;
    QList<Discord::Message> messages;
    bool reachedLatest = false;
    Snowflake anchorId = Snowflake::Invalid;
};

class MessageManager : public QObject
{
    Q_OBJECT
public:
    explicit MessageManager(Snowflake accountId, Discord::Client *client,
                            UserManager *userManager, QObject *parent = nullptr);
    ~MessageManager() override;

    void setChannelResolver(std::function<QString(Snowflake)> resolver);
    void setChannelLinkResolver(Markdown::ChannelLinkResolverFn resolver);
    void setEmojiManager(EmojiManager *manager);

    [[nodiscard]] QString channelTopicHtml(const QString &topic, Snowflake channelId) const;
    [[nodiscard]] QString profileBioHtml(const QString &bio) const;

    void requestLoadChannel(Snowflake channelId);
    void requestLoadHistory(Snowflake channelId, Snowflake beforeId);
    void requestLoadFuture(Snowflake channelId, Snowflake afterId);
    void requestLoadAround(Snowflake channelId, Snowflake messageId);
    void sendMessage(Snowflake channelId, const QString &content,
                     Snowflake replyToMessageId = Snowflake::Invalid,
                     const QList<PendingAttachment> &attachments = {});
    void cancelSend(Snowflake channelId, const QString &nonce);

    using ReactionLocation = Discord::Client::ReactionLocation;
    void addReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location);
    void removeReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location);

    enum class ReactionRejection {
        TooManyReactions,
        AlreadyReactedWithOtherType,
    };
    Q_ENUM(ReactionRejection)

signals:
    void reactionRejected(Acheron::Core::MessageManager::ReactionRejection reason);
    void messagesReceived(const MessageRequestResult &result);
    void messageErrored(const QString &nonce);
    void messageDeleted(Core::Snowflake channelId, Core::Snowflake messageId);
    void membersLearnedFromMessage(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds);
    void attachmentUploadProgress(const QString &nonce, int fileIndex, qint64 sent, qint64 total);

public slots:
    void onMessageCreated(const Discord::Message &message);
    void onMessageUpdated(const Discord::Message &message);
    void onMessageDeleted(const Discord::MessageDelete &event);
    void onMessageSendFailed(const QString &nonce, const QString &error);
    void onReactionAdd(const Discord::MessageReactionAdd &event);
    void onReactionAddMany(const Discord::MessageReactionAddMany &event);
    void onReactionRemove(const Discord::MessageReactionRemove &event);
    void onReactionRemoveAll(const Discord::MessageReactionRemoveAll &event);
    void onReactionRemoveEmoji(const Discord::MessageReactionRemoveEmoji &event);

private:
    using LoadType = Discord::Client::MessageLoadType;
    static constexpr int PageSize = 30;
    static constexpr int JumpWindow = 30;

    using PageFetcher = std::function<void(Discord::Client::MessagesCallback)>;
    void fetchPage(LoadType type, Snowflake channelId, Snowflake anchorId, const PageFetcher &fetch);
    void onApiMessagesReceived(const QList<Discord::Message> &messages, LoadType type,
                               Snowflake channelId, Snowflake anchorId = Snowflake::Invalid);
    [[nodiscard]] std::optional<QList<Discord::Message>> cachedSlice(Snowflake channelId,
                                                                     const MessageSegments::Run &run,
                                                                     int from, int to);
    void emitReactionUpdate(Discord::Message &msg);

    struct ReactionChange
    {
        Snowflake channelId;
        Snowflake messageId;
        Discord::Emoji emoji;
        bool isBurst;
        ReactionLocation location;
    };
    enum class ReactionOp {
        Add,
        Remove,
    };
    enum class ReactionAttempt {
        First,
        Retry,
    };
    void updateReactions(Snowflake messageId, const std::function<bool(QList<Discord::Reaction> &)> &mutate);
    void applyOwnReaction(ReactionOp op, const ReactionChange &change);
    void sendReactionChange(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt);
    void onReactionChangeFailed(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt, const Discord::Client::ReactionResult &result);
    void cacheGatewayMembers(const Discord::Message &msg);
    void parseMessageContent(Discord::Message &msg);
    void cacheParsedContent(Discord::Message &msg, const QString &content, Snowflake channelId) const;

    Storage::MessageRepository repo;

    Discord::Client *client;
    UserManager *userManager;
    EmojiManager *emojiManager = nullptr;
    std::unique_ptr<Markdown::Parser> parser;

    QCache<Snowflake, Discord::Message> messageCache;
    QHash<Snowflake, MessageSegments> segments;
    QSet<Snowflake> fetchedChannels;
    QHash<Snowflake, Snowflake> channelStartId; // nothing exists before this message
    QSet<QPair<Snowflake, LoadType>> pagesInFlight;
};

} // namespace Core
} // namespace Acheron
