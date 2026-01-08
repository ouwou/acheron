#pragma once

#include <QtWidgets>
#include "Core/Session.hpp"
#include "Core/MessageManager.hpp"

using Acheron::Core::ClientInstance;

namespace Acheron {

namespace Core {
class ImageManager;
class AttachmentCache;
} // namespace Core

struct AttachmentData
{
    QUrl proxyUrl;
    QUrl originalUrl;
    QSize displaySize;
    QPixmap pixmap;
    bool isLoading;
    bool isImage;
    QString filename;
    qint64 fileSizeBytes;
};

namespace UI {
class ChatModel : public QAbstractListModel
{
    Q_OBJECT
public:
    ChatModel(Core::ImageManager *imageManager, Core::AttachmentCache *attachmentCache,
              QObject *parent = nullptr);

    enum Roles {
        ContentRole = Qt::UserRole + 1,
        UsernameRole,
        UserIdRole,
        AvatarRole,
        TimestampRole,
        CachedSizeRole,
        ShowHeaderRole,
        DateSeparatorRole,
        HtmlRole,
        AttachmentsRole,
    };

    using AvatarUrlResolver = std::function<QUrl(const Discord::User &)>;
    void setAvatarUrlResolver(AvatarUrlResolver resolver);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

    [[nodiscard]] Snowflake getOldestMessageId() const;
    [[nodiscard]] Snowflake getActiveChannelId() const;

public slots:
    void setActiveChannel(Snowflake channelId);
    void handleIncomingMessages(const Core::MessageRequestResult &result);

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
    Core::AttachmentCache *attachmentCache;
    QVector<Discord::Message> messages;
    mutable QHash<Snowflake, QSize> sizeCache;

    Snowflake currentChannelId = Snowflake::Invalid;

    AvatarUrlResolver avatarUrlResolver;

    mutable QMultiMap<QUrl, QPersistentModelIndex> pendingRequests;
};
} // namespace UI
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::AttachmentData)
Q_DECLARE_METATYPE(QList<Acheron::AttachmentData>)
