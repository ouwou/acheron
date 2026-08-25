#include "ForumPostModel.hpp"

#include <QPixmap>
#include <QTextDocument>
#include <QTextOption>

#include "Core/ForumManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Markdown/Parser.hpp"
#include "Core/Theme/Manager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "UI/Chat/ChatLayout.hpp"

namespace Acheron {
namespace UI {

ForumPostModel::ForumPostModel(Core::ImageManager *imageManager, QObject *parent)
    : QAbstractListModel(parent), imageManager(imageManager)
{
    if (imageManager)
        connect(imageManager, &Core::ImageManager::imageFetched, this,
                [this](const QUrl &url, const QSize &size, const QPixmap &) {
                    if (!manager)
                        return;
                    if (size == QSize(kThumbFetchPx, kThumbFetchPx)) {
                        for (int row = 0; row < rows.size(); row++) {
                            if (thumbnailUrlOf(manager->firstMessagePtr(rows[row])) != url)
                                continue;
                            QModelIndex idx = index(row);
                            emit dataChanged(idx, idx, { ThumbnailRole });
                        }
                    } else if (size == QSize(kTagEmojiPx, kTagEmojiPx) && isTagEmojiUrl(url)) {
                        if (!rows.isEmpty())
                            emit dataChanged(index(0), index(rows.size() - 1), { TagsRole });
                    }
                });

    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged, this,
            [this]() {
                previewDocCache.clear();
                if (!rows.isEmpty())
                    emit dataChanged(index(0), index(rows.size() - 1));
            });
}

void ForumPostModel::setAccount(Snowflake id)
{
    accountId = id;
}

void ForumPostModel::setManager(Core::ForumManager *m)
{
    if (manager == m)
        return;
    if (manager)
        disconnect(manager, nullptr, this, nullptr);
    manager = m;
    if (manager) {
        connect(manager, &Core::ForumManager::postsReset, this, &ForumPostModel::onPostsReset);
        connect(manager, &Core::ForumManager::postsAppended, this, &ForumPostModel::onPostsAppended);
        connect(manager, &Core::ForumManager::postInserted, this, &ForumPostModel::onPostInserted);
        connect(manager, &Core::ForumManager::postUpdated, this, &ForumPostModel::onPostUpdated);
        connect(manager, &Core::ForumManager::postRemoved, this, &ForumPostModel::onPostRemoved);
    }
}

void ForumPostModel::resetRows()
{
    beginResetModel();
    rebuildTagLookup();
    syncRowsFromManager();
    previewContentCache.clear();
    previewDocCache.clear();
    endResetModel();
}

void ForumPostModel::setForum(Snowflake forumId, Snowflake guildId)
{
    currentForumId = forumId;
    currentGuildId = guildId;
    resetRows();

    if (manager)
        manager->openForum(forumId);
}

void ForumPostModel::setDisplayNameResolver(DisplayNameResolver resolver)
{
    displayNameResolver = std::move(resolver);
}

void ForumPostModel::setRoleColorResolver(RoleColorResolver resolver)
{
    roleColorResolver = std::move(resolver);
}

void ForumPostModel::refreshPost(Snowflake threadId)
{
    int row = rows.indexOf(threadId);
    if (row < 0)
        return;
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
}

void ForumPostModel::refreshAuthors()
{
    if (!rows.isEmpty())
        emit dataChanged(index(0), index(rows.size() - 1), { AuthorRole, AuthorColorRole });
}

void ForumPostModel::rebuildTagLookup()
{
    tagInfo.clear();
    if (!manager)
        return;
    for (const auto &tag : manager->availableTags(currentForumId)) {
        if (!tag.id.hasValue() || !tag.name.hasValue())
            continue;

        TagInfo info;
        info.name = tag.name.get();
        if (tag.emojiId.hasValue() && tag.emojiId.get().isValid())
            info.emojiUrl = Discord::Cdn::emoji(tag.emojiId.get(), kTagEmojiPx);
        else if (tag.emojiName.hasValue())
            info.emoji = tag.emojiName.get();
        tagInfo.insert(tag.id.get(), info);
    }
}

QPixmap ForumPostModel::fetchedPixmap(const QUrl &url, const QSize &size) const
{
    if (!imageManager)
        return {};
    QPixmap px = imageManager->get(url, size, accountId);
    return imageManager->isCached(url, size) ? px : QPixmap();
}

bool ForumPostModel::isTagEmojiUrl(const QUrl &url) const
{
    for (auto it = tagInfo.constBegin(); it != tagInfo.constEnd(); ++it)
        if (it->emojiUrl == url)
            return true;
    return false;
}

void ForumPostModel::syncRowsFromManager()
{
    rows.clear();
    if (!manager)
        return;
    for (const auto &post : manager->posts(currentForumId))
        rows.append(post.id.get());
}

int ForumPostModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return rows.size();
}

QVariant ForumPostModel::data(const QModelIndex &index, int role) const
{
    if (!manager || !index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= rows.size())
        return {};

    const Snowflake threadId = rows[row];
    const Discord::Channel *postPtr = manager->post(threadId);
    if (!postPtr)
        return {};
    const Discord::Channel &post = *postPtr;

    switch (role) {
    case TitleRole:
        return post.name.hasValue() ? post.name.get() : QString();
    case HasTagsRole:
        return hasTags(post);
    case TagsRole: {
        QList<ForumTagChip> chips;
        if (post.appliedTags.hasValue())
            for (Snowflake tagId : post.appliedTags.get()) {
                auto it = tagInfo.constFind(tagId);
                if (it == tagInfo.constEnd())
                    continue;

                ForumTagChip chip;
                chip.name = it->name;
                chip.emoji = it->emoji;
                chip.hasImage = !it->emojiUrl.isEmpty();
                if (chip.hasImage)
                    chip.image = fetchedPixmap(it->emojiUrl, QSize(kTagEmojiPx, kTagEmojiPx));
                chips.append(chip);
            }
        return QVariant::fromValue(chips);
    }
    case MessageCountRole:
        return post.messageCount.hasValue() ? post.messageCount.get() : 0;
    case AuthorRole:
        return authorName(threadId);
    case AuthorColorRole:
        return authorColor(threadId);
    case UnreadRole:
        return manager->postReadState(threadId).unread;
    case IsNewRole:
        return manager->postReadState(threadId).isNew;
    case UnreadTextRole: {
        manager->ensureUnreadCount(currentForumId, threadId);
        const auto marker = manager->unreadMarker(threadId);
        if (!marker.show)
            return QString();
        if (marker.count <= 0)
            return QStringLiteral("1+"); // unread but we dont know yet
        if (marker.count >= kMaxUnreadShown)
            return QString::number(kMaxUnreadShown) + QStringLiteral("+");
        return QString::number(marker.count);
    }
    case ReactionEmojiRole: {
        const Discord::Message *msg = starterOf(threadId);
        if (msg && msg->reactions.hasValue() && !msg->reactions.get().isEmpty()) {
            const Discord::Emoji &e = msg->reactions.get().first().emoji.get();
            if (!e.id.hasValue() || !e.id.get().isValid())
                return e.name.hasValue() ? e.name.get() : QString();
        }
        return QString();
    }
    case ReactionCountRole: {
        const Discord::Message *msg = starterOf(threadId);
        if (msg && msg->reactions.hasValue() && !msg->reactions.get().isEmpty())
            return msg->reactions.get().first().count.get();
        return 0;
    }
    case TimestampRole:
        return post.effectiveLastMessageId().toDateTime();
    case ThreadIdRole:
        return QVariant::fromValue<quint64>(threadId);
    case GuildIdRole:
        return QVariant::fromValue<quint64>(currentGuildId);
    case HasMediaRole:
        return !thumbnailUrlOf(starterOf(threadId)).isEmpty();
    case ThumbnailRole: {
        QUrl url = thumbnailUrlOf(starterOf(threadId));
        if (url.isEmpty())
            return {};
        QPixmap px = fetchedPixmap(url, QSize(kThumbFetchPx, kThumbFetchPx));
        return px.isNull() ? QVariant() : QVariant(px);
    }
    }
    return {};
}

const Discord::Message *ForumPostModel::starterOf(Snowflake threadId) const
{
    if (!manager)
        return nullptr;
    manager->ensureStarter(currentForumId, threadId);
    return manager->firstMessagePtr(threadId);
}

bool ForumPostModel::hasTags(const Discord::Channel &post) const
{
    if (!post.appliedTags.hasValue())
        return false;
    for (Snowflake tagId : post.appliedTags.get())
        if (tagInfo.contains(tagId))
            return true;
    return false;
}

QUrl ForumPostModel::thumbnailUrlOf(const Discord::Message *msg)
{
    if (!msg)
        return {};

    if (msg->attachments.hasValue())
        for (const auto &att : msg->attachments.get()) {
            if (!att.isImage())
                continue;
            if (att.proxyUrl.hasValue() && !att.proxyUrl.get().isEmpty())
                return QUrl(att.proxyUrl.get());
            if (att.url.hasValue() && !att.url.get().isEmpty())
                return QUrl(att.url.get());
        }

    if (msg->embeds.hasValue())
        for (const auto &embed : msg->embeds.get()) {
            const Discord::EmbedMedia *media = nullptr;
            if (embed.image.hasValue())
                media = &embed.image.get();
            else if (embed.thumbnail.hasValue())
                media = &embed.thumbnail.get();
            if (!media)
                continue;
            if (media->proxyUrl.hasValue() && !media->proxyUrl.get().isEmpty())
                return QUrl(media->proxyUrl.get());
            if (media->url.hasValue() && !media->url.get().isEmpty())
                return QUrl(media->url.get());
        }

    return {};
}

QString ForumPostModel::authorName(Snowflake threadId) const
{
    const Discord::Message *msg = starterOf(threadId);
    if (!msg || !msg->author.hasValue())
        return {};
    const Discord::User &author = msg->author.get();
    if (displayNameResolver) {
        QString name = displayNameResolver(author.id.get(), currentGuildId);
        if (!name.isEmpty())
            return name;
    }
    return author.getDisplayName();
}

QString ForumPostModel::previewHtml(Snowflake threadId) const
{
    auto cached = previewContentCache.constFind(threadId);
    if (cached != previewContentCache.constEnd())
        return cached.value();

    QString contentHtml;
    const Discord::Message *msg = starterOf(threadId);
    if (msg && msg->content.hasValue() && !msg->content.get().isEmpty()) {
        QString content = msg->content.get();
        content.replace('\n', ' ');

        static Core::Markdown::Parser parser;
        Core::Markdown::ParseState state;
        state.isInline = true;
        state.excludedRules.insert("link");
        state.excludedRules.insert("url");
        state.excludedRules.insert("autolink");
        contentHtml = parser.toHtml(parser.parse(content, state));
    }
    previewContentCache.insert(threadId, contentHtml);
    return contentHtml;
}

QColor ForumPostModel::authorColor(Snowflake threadId) const
{
    if (!roleColorResolver)
        return {};
    const Discord::Message *msg = starterOf(threadId);
    if (!msg || !msg->author.hasValue())
        return {};
    return roleColorResolver(msg->author.get().id.get(), currentGuildId);
}

QTextDocument *ForumPostModel::previewDocument(Snowflake threadId, const QFont &font) const
{
    QTextDocument *doc = previewDocCache.object(threadId);
    if (doc && doc->defaultFont() == font)
        return doc;

    const QString html = previewHtml(threadId);
    if (html.isEmpty())
        return nullptr;

    doc = new QTextDocument;
    doc->setDefaultFont(font);
    doc->setDefaultStyleSheet(ChatLayout::richTextStyleSheet());
    doc->setDocumentMargin(0);
    QTextOption noWrap;
    noWrap.setWrapMode(QTextOption::NoWrap);
    doc->setDefaultTextOption(noWrap);
    doc->setHtml(html);
    previewDocCache.insert(threadId, doc);
    return doc;
}

void ForumPostModel::invalidatePreview(Snowflake threadId) const
{
    previewContentCache.remove(threadId);
    previewDocCache.remove(threadId);
}

bool ForumPostModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid() || !manager || !currentForumId.isValid())
        return false;
    return manager->hasMore(currentForumId) && !manager->isLoading(currentForumId);
}

void ForumPostModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || !manager)
        return;
    manager->loadMorePosts(currentForumId);
}

void ForumPostModel::onPostsReset(Snowflake forumId)
{
    if (forumId == currentForumId)
        resetRows();
}

void ForumPostModel::onPostsAppended(Snowflake forumId, int startRow, int count)
{
    if (forumId != currentForumId || count <= 0)
        return;
    const auto &posts = manager->posts(currentForumId);
    beginInsertRows({}, startRow, startRow + count - 1);
    for (int i = startRow; i < startRow + count && i < posts.size(); i++)
        rows.append(posts[i].id.get());
    endInsertRows();
}

void ForumPostModel::onPostInserted(Snowflake forumId, Snowflake threadId, int row)
{
    if (forumId != currentForumId)
        return;
    if (row < 0 || row > rows.size())
        row = rows.size();
    beginInsertRows({}, row, row);
    rows.insert(row, threadId);
    endInsertRows();
}

void ForumPostModel::onPostUpdated(Snowflake forumId, Snowflake threadId)
{
    if (forumId != currentForumId)
        return;
    int row = rows.indexOf(threadId);
    if (row < 0)
        return;
    invalidatePreview(threadId);
    QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
}

void ForumPostModel::onPostRemoved(Snowflake forumId, Snowflake threadId)
{
    if (forumId != currentForumId)
        return;
    int row = rows.indexOf(threadId);
    if (row < 0)
        return;
    invalidatePreview(threadId);
    beginRemoveRows({}, row, row);
    rows.removeAt(row);
    endRemoveRows();
}

} // namespace UI
} // namespace Acheron
