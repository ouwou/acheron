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
    QRect nameRect;
    QRect valueRect;
    int fieldHeight;
};

struct EmbedLayout
{
    QRect embedRect;
    QRect contentRect;
    int contentWidth;
    bool hasThumbnail;

    int thumbnailY;
    int providerY;
    int authorY;
    int titleY;
    int descriptionY;
    int fieldsY;
    int imagesY;
    int footerY;

    int providerHeight;
    int authorHeight;
    int titleHeight;
    int descriptionHeight;
    int fieldsHeight;
    int imagesHeight;
    int footerHeight;

    int totalHeight;
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
    QRect avatarRect;
    QRect headerRect;
    QRect textRect;

    bool showHeader;
    bool hasSeparator;

    int textHeight;

    int attachmentsTop;
    QList<AttachmentLayout> imageLayouts;
    QList<AttachmentLayout> fileLayouts;
    AttachmentGridLayout imageGrid;
    int attachmentsTotalHeight;

    int embedsTop;
    QList<EmbedLayout> embedLayouts;
    int embedsTotalHeight;

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
};

MessageLayout calculateMessageLayout(const LayoutContext &ctx);
EmbedLayout calculateEmbedLayout(const EmbedData &embed, const QFont &font, int maxWidth, int top);
int calculateAttachmentsHeight(const QList<AttachmentData> &attachments, int textWidth);
int calculateEmbedsHeight(const QList<EmbedData> &embeds, const QFont &font, int textWidth);
enum class EmbedHitType { None, Title, Author, Image, VideoThumbnail };

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

QString formatFileSize(qint64 bytes);

void drawCroppedPixmap(QPainter *painter, const QRect &targetRect, const QPixmap &pixmap);

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
