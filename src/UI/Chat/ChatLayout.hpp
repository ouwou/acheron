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

QRect dateSeparatorRectForRow(const QRect &rowRect);
QFont getFontForIndex(const QAbstractItemView *view, const QModelIndex &index);
QRect avatarRectForRow(const QRect &rowRect, bool hasSeperator);
QRect headerRectForRow(const QRect &rowRect, const QFontMetrics &fm, bool hasSeperator);
QRect textRectForRow(const QRect &rowRect, bool showHeader, const QFontMetrics &fm,
                     bool hasSeperator);
void setupDocument(QTextDocument &doc, const QString &htmlContent, const QFont &font,
                   int textWidth);
int hitTestCharIndex(QAbstractItemView *view, const QModelIndex &index, const QPoint &viewportPos);
QRectF charRectInDocument(const QTextDocument &doc, int charIndex);
QString getLinkAt(const QAbstractItemView *view, const QModelIndex &index, const QPoint &mousePos);
std::optional<AttachmentData> getAttachmentAt(const QAbstractItemView *view,
                                              const QModelIndex &index, const QPoint &mousePos);

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

enum class EmbedHitType { None, Title, Author, Image, VideoThumbnail };

struct EmbedHitResult
{
    int embedIndex = -1;
    EmbedHitType hitType = EmbedHitType::None;
    QString url;
    QPixmap image;
    QSize imageSize;
};

AttachmentGridLayout calculateAttachmentGrid(int count, int maxWidth);
QString formatFileSize(qint64 bytes);
std::optional<EmbedHitResult> getEmbedAt(const QAbstractItemView *view, const QModelIndex &index,
                                         const QPoint &mousePos);

} // namespace ChatLayout
} // namespace UI
} // namespace Acheron
