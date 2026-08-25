#pragma once

#include <QAbstractListModel>
#include <QCache>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QUrl>

#include <functional>

#include "Core/Snowflake.hpp"

class QTextDocument;

namespace Acheron {

namespace Discord {
struct Message;
struct Channel;
} // namespace Discord

namespace Core {
class ForumManager;
class ImageManager;
} // namespace Core

namespace UI {

using Acheron::Core::Snowflake;

struct ForumTagChip
{
    QString name;
    QString emoji;
    QPixmap image;
    bool hasImage = false;
};

class ForumPostModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        TagsRole,
        AuthorRole,
        AuthorColorRole,
        MessageCountRole,
        TimestampRole,
        ReactionEmojiRole,
        ReactionCountRole,
        ThreadIdRole,
        GuildIdRole,
        ThumbnailRole,
        HasMediaRole,
        HasTagsRole,
        UnreadRole,
        IsNewRole,
        UnreadTextRole,
    };

    explicit ForumPostModel(Core::ImageManager *imageManager, QObject *parent = nullptr);

    using DisplayNameResolver = std::function<QString(Snowflake, Snowflake)>;
    using RoleColorResolver = std::function<QColor(Snowflake, Snowflake)>;
    void setManager(Core::ForumManager *manager);
    void setAccount(Snowflake accountId);
    void setForum(Snowflake forumId, Snowflake guildId);
    void setDisplayNameResolver(DisplayNameResolver resolver);
    void setRoleColorResolver(RoleColorResolver resolver);
    void refreshAuthors();
    void refreshPost(Snowflake threadId);

    QTextDocument *previewDocument(Snowflake threadId, const QFont &font) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

private slots:
    void onPostsReset(Snowflake forumId);
    void onPostsAppended(Snowflake forumId, int startRow, int count);
    void onPostInserted(Snowflake forumId, Snowflake threadId, int row);
    void onPostUpdated(Snowflake forumId, Snowflake threadId);
    void onPostRemoved(Snowflake forumId, Snowflake threadId);

private:
    void rebuildTagLookup();
    void syncRowsFromManager();
    void resetRows();
    [[nodiscard]] const Discord::Message *starterOf(Snowflake threadId) const;
    [[nodiscard]] static QUrl thumbnailUrlOf(const Discord::Message *msg);
    [[nodiscard]] bool hasTags(const Discord::Channel &post) const;
    [[nodiscard]] QString previewHtml(Snowflake threadId) const;
    [[nodiscard]] QString authorName(Snowflake threadId) const;
    [[nodiscard]] QColor authorColor(Snowflake threadId) const;
    [[nodiscard]] bool isTagEmojiUrl(const QUrl &url) const;
    [[nodiscard]] QPixmap fetchedPixmap(const QUrl &url, const QSize &size) const;
    void invalidatePreview(Snowflake threadId) const;

    static constexpr int kThumbFetchPx = 160;
    static constexpr int kTagEmojiPx = 32;
    static constexpr int kMaxUnreadShown = 25;

    Core::ForumManager *manager = nullptr;
    Core::ImageManager *imageManager = nullptr;
    Snowflake accountId;
    DisplayNameResolver displayNameResolver;
    RoleColorResolver roleColorResolver;
    Snowflake currentForumId;
    Snowflake currentGuildId;
    QList<Snowflake> rows;

    struct TagInfo
    {
        QString name;
        QString emoji;
        QUrl emojiUrl;
    };
    QHash<Snowflake, TagInfo> tagInfo;
    mutable QHash<Snowflake, QString> previewContentCache;
    mutable QCache<Snowflake, QTextDocument> previewDocCache;
};

} // namespace UI
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::UI::ForumTagChip)
Q_DECLARE_METATYPE(QList<Acheron::UI::ForumTagChip>)
