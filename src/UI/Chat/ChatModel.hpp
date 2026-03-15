#pragma once

#include <QtWidgets>
#include <QCache>
#include <QTextDocument>
#include "Core/Session.hpp"
#include "Core/MessageManager.hpp"

using Acheron::Core::ClientInstance;

namespace Acheron {

namespace Core {
class ImageManager;
} // namespace Core

struct AttachmentData
{
    Snowflake id;
    QUrl proxyUrl;
    QUrl originalUrl;
    QSize displaySize;
    QPixmap pixmap;
    bool isLoading;
    bool isImage;
    QString filename;
    qint64 fileSizeBytes;
    bool isSpoiler;
};

struct EmbedFieldData
{
    QString name;
    QString value;
    QString nameParsed;
    QString valueParsed;
    bool isInline = false;
};

struct EmbedImageData
{
    QUrl url;
    QPixmap pixmap;
    QSize displaySize;
};

enum class EmbedType {
    AgeVerificationSystemNotification,
    ApplicationNews,
    Article,
    AutoModerationMessage,
    AutoModerationNotification,
    Gift,
    Gifv,
    Image,
    Link,
    PollResult,
    PostPreview,
    Rich,
    SafetyPolicyNotice,
    SafetySystemNotification,
    Video,
};

struct ReplyData
{
    enum class State {
        None, // not a reply
        Present, // referenced message is available
        Deleted, // referenced message was deleted
        Unknown, // backend didn't fetch referenced message
    };

    State state = State::None;
    Core::Snowflake referencedMessageId;
    QString authorName;
    QColor authorColor;
    Core::Snowflake authorId;
    QString contentSnippet;
};

struct ReactionData
{
    QString emojiName;
    Core::Snowflake emojiId;
    bool emojiAnimated = false;
    int count = 0;
    bool me = false;
    bool isBurst = false;
    QPixmap emojiPixmap;
    bool isLoading = false;
    QColor burstTintColor;
};

struct EmbedData
{
    EmbedType type = EmbedType::Rich; // should this be default idk
    QString title;
    QString description;
    QString titleParsed;
    QString descriptionParsed;
    QString url;
    QDateTime timestamp;
    QColor color;

    QString authorName;
    QString authorUrl;
    QUrl authorIconUrl;
    QPixmap authorIcon;

    QString footerText;
    QUrl footerIconUrl;
    QPixmap footerIcon;

    QUrl thumbnailUrl;
    QPixmap thumbnail;
    QSize thumbnailSize;

    QList<EmbedImageData> images;

    QUrl videoThumbnailUrl;
    QPixmap videoThumbnail;
    QSize videoThumbnailSize;

    QString providerName;
    QString providerUrl;

    QList<EmbedFieldData> fields;
};

namespace UI {

struct DocCacheKey
{
    Snowflake messageId;
    // 0 = body, then embed/field sub-documents encoded as:
    // (embedIndex+1)*1000 + offset (0=title, 1=desc, 100+fieldIndex*2=fieldName, 100+fieldIndex*2+1=fieldValue)
    int subId = 0;

    bool operator==(const DocCacheKey &o) const
    {
        return messageId == o.messageId && subId == o.subId;
    }
};

inline size_t qHash(const DocCacheKey &k, size_t seed = 0)
{
    return qHashMulti(seed, quint64(k.messageId), k.subId);
}

inline DocCacheKey bodyDocKey(Snowflake msgId)
{
    return { msgId, 0 };
}
inline DocCacheKey embedTitleDocKey(Snowflake msgId, int embedIdx)
{
    return { msgId, (embedIdx + 1) * 1000 };
}
inline DocCacheKey embedDescDocKey(Snowflake msgId, int embedIdx)
{
    return { msgId, (embedIdx + 1) * 1000 + 1 };
}
inline DocCacheKey embedFieldNameDocKey(Snowflake msgId, int embedIdx, int fieldIdx)
{
    return { msgId, (embedIdx + 1) * 1000 + 100 + fieldIdx * 2 };
}
inline DocCacheKey embedFieldValueDocKey(Snowflake msgId, int embedIdx, int fieldIdx)
{
    return { msgId, (embedIdx + 1) * 1000 + 100 + fieldIdx * 2 + 1 };
}

class ChatModel : public QAbstractListModel
{
    Q_OBJECT
public:
    ChatModel(Core::ImageManager *imageManager, QObject *parent = nullptr);

    enum Roles {
        ContentRole = Qt::UserRole + 1,
        UsernameRole,
        UserIdRole,
        AvatarRole,
        TimestampRole,
        EditedTimestampRole,
        CachedSizeRole,
        ShowHeaderRole,
        DateSeparatorRole,
        HtmlRole,
        AttachmentsRole,
        EmbedsRole,
        IsPendingRole,
        IsErroredRole,
        UsernameColorRole,
        MessageIdRole,
        ReplyDataRole,
        ReactionsRole,
        IsSystemMessageRole,
    };

    using AvatarUrlResolver = std::function<QUrl(const Discord::User &)>;
    void setAvatarUrlResolver(AvatarUrlResolver resolver);

    using DisplayNameResolver = std::function<QString(Snowflake userId, Snowflake guildId)>;
    void setDisplayNameResolver(DisplayNameResolver resolver);

    using RoleColorResolver = std::function<QColor(Snowflake userId, Snowflake guildId)>;
    void setRoleColorResolver(RoleColorResolver resolver);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

    [[nodiscard]] Snowflake getOldestMessageId() const;
    [[nodiscard]] Snowflake getActiveChannelId() const;
    [[nodiscard]] bool isSpoilerRevealed(Snowflake attachmentId) const;

    QTextDocument *getCachedDocument(const DocCacheKey &key) const;
    void cacheDocument(const DocCacheKey &key, QTextDocument *doc) const;
    void invalidateDocCache();
    void invalidateDocCacheForMessage(Snowflake messageId);

public slots:
    void setActiveChannel(Snowflake channelId, Snowflake guildId = Snowflake::Invalid);
    void handleIncomingMessages(const Core::MessageRequestResult &result);
    void handleMessageDeleted(Snowflake channelId, Snowflake messageId);
    void handleMessageErrored(const QString &nonce);
    void refreshUsersInView(const QList<Snowflake> &userIds);
    void revealSpoiler(Snowflake attachmentId);

    void triggerResize(int row)
    {
        if (row < 0 || row >= rowCount())
            return;
        QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, { CachedSizeRole });
    }

private:
    void setMessages(const QList<Discord::Message> &messages);

    Core::ImageManager *imageManager;
    QVector<Discord::Message> messages;
    mutable QHash<Snowflake, QSize> sizeCache;
    mutable QHash<Snowflake, QList<EmbedData>> embedCache;
    mutable QCache<DocCacheKey, QTextDocument> docCache{ 500 };
    mutable int docCacheWidth = 0;

    Snowflake currentChannelId = Snowflake::Invalid;
    Snowflake currentGuildId = Snowflake::Invalid;

    AvatarUrlResolver avatarUrlResolver;
    DisplayNameResolver displayNameResolver;
    RoleColorResolver roleColorResolver;

    mutable QMultiMap<QUrl, QPersistentModelIndex> pendingRequests;
    QSet<QString> pendingNonces;
    QSet<QString> erroredNonces;
    mutable QSet<Snowflake> revealedSpoilers;
    mutable bool suppressImageFetch = false;

    friend class ChatDelegate;
};
} // namespace UI
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::ReplyData)
Q_DECLARE_METATYPE(Acheron::AttachmentData)
Q_DECLARE_METATYPE(QList<Acheron::AttachmentData>)
Q_DECLARE_METATYPE(Acheron::EmbedFieldData)
Q_DECLARE_METATYPE(Acheron::EmbedImageData)
Q_DECLARE_METATYPE(Acheron::EmbedData)
Q_DECLARE_METATYPE(QList<Acheron::EmbedData>)
Q_DECLARE_METATYPE(Acheron::ReactionData)
Q_DECLARE_METATYPE(QList<Acheron::ReactionData>)
