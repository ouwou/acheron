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
constexpr int videoDownloadBtnSize() noexcept
{
    return 24;
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
    QRect downloadRect;
    bool isImage;
    int index;
};

struct HitRegion
{
    enum class Kind {
        Avatar,
        UsernameHeader,
        ReplyBar,
        AttachmentImage,
        AttachmentVideo,
        AttachmentVideoDownload,
        AttachmentFile,
        EmbedThumbnail,
        EmbedAuthor,
        EmbedTitle,
        EmbedImage,
        EmbedVideoThumbnail,
        EmbedDescription,
        EmbedFieldName,
        EmbedFieldValue,
        Reaction,

        TextLink,
        TextCursor,
        EmbedLink,
    };

    Kind kind;
    QRect rect;
    int index = -1;
    int subIndex = -1;
    QString url;
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

    QList<HitRegion> hitRegions;
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

struct ResolvedLayout
{
    MessageLayout layout;
    LayoutContext ctx;
};

ResolvedLayout resolveLayout(const QAbstractItemView *view, const QModelIndex &index);
EmbedLayout calculateEmbedLayout(const EmbedData &embed, const QFont &font, int maxWidth, int left,
                                 int top, const ChatModel *model = nullptr,
                                 Core::Snowflake messageId = Core::Snowflake::Invalid,
                                 int embedIndex = -1);
int calculateAttachmentsHeight(const QList<AttachmentData> &attachments, int textWidth);
int calculateEmbedsHeight(const QList<EmbedData> &embeds, const QFont &font, int textWidth);

QRect dateSeparatorRectForRow(const QRect &rowRect);

QString richTextStyleSheet();
void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font,
                   int textWidth);
QRectF charRectInDocument(const QTextDocument &doc, int charIndex);

std::optional<HitRegion> hitTest(const ResolvedLayout &resolved, const QPoint &mousePos);

int hitTestCharIndex(const ResolvedLayout &resolved, const QPoint &viewportPos);
QString getLinkAt(const ResolvedLayout &resolved, const QPoint &mousePos);
int hitTestCharIndex(const QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos);

QString formatFileSize(qint64 bytes);

void drawCroppedPixmap(QPainter *painter, const QRect &targetRect, const QPixmap &pixmap);
QPixmap createBlurredPixmap(const QPixmap &source, int blurRadius = 30);

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron