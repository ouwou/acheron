#pragma once

#include <QRect>
#include <QPoint>
#include <QTextDocument>
#include <QAbstractItemView>
#include <QModelIndex>

#include "ChatModel.hpp"

namespace Acheron {
namespace UI {
namespace ChatLayout {

constexpr int padding() noexcept
{
    return 8;
}
constexpr int blockTopPadding() noexcept
{
    return 14;
}
constexpr int avatarSize() noexcept
{
    return 32;
}
constexpr int separatorHeight() noexcept
{
    return 24;
}
constexpr int embedMaxWidth() noexcept
{
    return 400;
}
constexpr int embedBorderWidth() noexcept
{
    return 4;
}
constexpr int embedPadding() noexcept
{
    return 12;
}
constexpr int thumbnailSize() noexcept
{
    return 80;
}
constexpr int authorIconSize() noexcept
{
    return 24;
}
constexpr int footerIconSize() noexcept
{
    return 16;
}
constexpr int fieldSpacing() noexcept
{
    return 8;
}
constexpr int fileAttachmentHeight() noexcept
{
    return 48;
}
constexpr int maxAttachmentWidth() noexcept
{
    return 400;
}
constexpr int replyBarHeight() noexcept
{
    return 18;
}
constexpr int replyBarSpacing() noexcept
{
    return 4;
}
constexpr int reactionPillHeight() noexcept
{
    return 22;
}
constexpr int reactionPillPadding() noexcept
{
    return 6;
}
constexpr int reactionEmojiSize() noexcept
{
    return 16;
}
constexpr int reactionSpacing() noexcept
{
    return 4;
}
constexpr int reactionRowSpacing() noexcept
{
    return 4;
}
constexpr int reactionTopMargin() noexcept
{
    return 4;
}

struct AttachmentGridCell
{
    int attachmentIndex;
    QRect rect;
};

struct AttachmentGridLayout
{
    QList<AttachmentGridCell> cells;
    int totalHeight;
};

AttachmentGridLayout calculateAttachmentGrid(int count, int maxWidth);

struct EmbedFieldLayout
{
    int fieldIndex;
    QRect nameRect;
    QRect valueRect;
};

struct EmbedImageLayout
{
    int imageIndex;
    QRect rect;
};

struct EmbedLayout
{
    QRect embedRect;
    QRect contentRect;
    int contentWidth;
    bool hasThumbnail;

    QRect thumbnailRect;
    QRect providerRect;
    QRect authorRect;
    QRect titleRect;
    QRect descriptionRect;
    QRect imagesRect;
    QRect footerRect;

    QList<EmbedFieldLayout> fieldLayouts;
    QList<EmbedImageLayout> imageLayouts;

    int totalHeight;
};

struct ReactionLayout
{
    int reactionIndex;
    QRect pillRect;
    QRect emojiRect;
    QRect countRect;
};

struct AttachmentLayout
{
    QRect rect;
    bool isImage;
    int index;
};

struct MessageLayout
{
    QRect rowRect;
    QRect separatorRect;
    QRect replyRect;
    QRect avatarRect;
    QRect headerRect;
    QRect textRect;

    bool showHeader;
    bool hasSeparator;
    bool hasReply = false;

    int textHeight;

    int attachmentsTop;
    QList<AttachmentLayout> imageLayouts;
    QList<AttachmentLayout> fileLayouts;
    AttachmentGridLayout imageGrid;
    int attachmentsTotalHeight;

    int embedsTop;
    QList<EmbedLayout> embedLayouts;
    int embedsTotalHeight;

    int reactionsTop;
    QList<ReactionLayout> reactionLayouts;
    int reactionsTotalHeight;

    int totalHeight;
};

struct LayoutContext
{
    QFont font;
    int rowWidth;
    int rowTop = 0;

    // Data from model
    bool showHeader;
    bool hasSeparator;
    QString htmlContent;
    QList<AttachmentData> attachments;
    QList<EmbedData> embeds;
    QList<ReactionData> reactions;
    ReplyData replyData;

    bool isSystemMessage = false;
    const ChatModel *model = nullptr;
    Core::Snowflake messageId;
};

MessageLayout calculateMessageLayout(const LayoutContext &ctx);
EmbedLayout calculateEmbedLayout(const EmbedData &embed, const QFont &font, int maxWidth, int left,
                                 int top, const ChatModel *model = nullptr,
                                 Core::Snowflake messageId = Core::Snowflake::Invalid,
                                 int embedIndex = -1);
int calculateAttachmentsHeight(const QList<AttachmentData> &attachments, int textWidth);
int calculateEmbedsHeight(const QList<EmbedData> &embeds, const QFont &font, int textWidth);
enum class EmbedHitType {
    None,
    Title,
    Author,
    Image,
    VideoThumbnail,
    Link,
};

struct EmbedHitResult
{
    int embedIndex = -1;
    EmbedHitType hitType = EmbedHitType::None;
    QString url;
    QPixmap image;
    QSize imageSize;
};

QRect dateSeparatorRectForRow(const QRect &rowRect);
QRect avatarRectForRow(const QRect &rowRect, bool hasSeparator);
QRect headerRectForRow(const QRect &rowRect, const QFontMetrics &fm, bool hasSeparator);
QRect textRectForRow(const QRect &rowRect, bool showHeader, const QFontMetrics &fm,
                     bool hasSeparator);

void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font,
                   int textWidth);
QFont getFontForIndex(const QAbstractItemView *view, const QModelIndex &index);

int hitTestCharIndex(QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos);
QRectF charRectInDocument(const QTextDocument &doc, int charIndex);
QString getLinkAt(const QAbstractItemView *view, const QModelIndex &index, const QPoint &mousePos);
std::optional<AttachmentData> getAttachmentAt(const QAbstractItemView *view,
                                              const QModelIndex &index, const QPoint &mousePos);
std::optional<EmbedHitResult> getEmbedAt(const QAbstractItemView *view, const QModelIndex &index,
                                         const QPoint &mousePos);

struct ReactionHitResult
{
    int reactionIndex = -1;
    ReactionData reaction;
};

std::optional<ReactionHitResult> getReactionAt(const QAbstractItemView *view,
                                               const QModelIndex &index, const QPoint &mousePos);

QString formatFileSize(qint64 bytes);

void drawCroppedPixmap(QPainter *painter, const QRect &targetRect, const QPixmap &pixmap);
QPixmap createBlurredPixmap(const QPixmap &source, int blurRadius = 30);

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
