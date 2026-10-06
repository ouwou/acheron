#include "ChannelTopicLine.hpp"

#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QTextLayout>

#include <cmath>

#include "UI/Chat/MessageTextDocument.hpp"

namespace Acheron {
namespace UI {

namespace {
constexpr int Unwrapped = -1;
}

ChannelTopicLine::ChannelTopicLine(Core::ImageManager *images, QWidget *parent)
    : QWidget(parent), images(images)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setForegroundRole(QPalette::PlaceholderText);
}

void ChannelTopicLine::setTopicHtml(const QString &html, Core::Snowflake accountId)
{
    delete document;
    document = nullptr;
    if (!html.isEmpty()) {
        document = new MessageTextDocument(images, accountId, this);
        document->setMessageHtml(html, font(), Unwrapped);
        connect(document, &MessageTextDocument::imageLoaded, this, qOverload<>(&QWidget::update));
    }
    setCursor(document ? Qt::PointingHandCursor : Qt::ArrowCursor);
    updateGeometry();
    update();
}

QSize ChannelTopicLine::sizeHint() const
{
    const int textHeight = fontMetrics().height();
    return QSize(0, document ? qMax(textHeight, int(std::ceil(document->size().height()))) : textHeight);
}

void ChannelTopicLine::paintEvent(QPaintEvent *)
{
    if (!document)
        return;

    QPainter painter(this);
    const QColor textColor = palette().color(foregroundRole());
    const qreal top = (height() - document->size().height()) / 2.0;

    const QString ellipsis = QStringLiteral("…");
    const QTextBlock block = document->firstBlock();
    const QTextLine line = block.layout()->lineCount() > 0 ? block.layout()->lineAt(0) : QTextLine();
    const bool overflows = line.isValid() && document->idealWidth() > width();
    qreal visibleWidth = width();
    if (overflows) {
        const int firstHiddenCharacter = line.xToCursor(width() - fontMetrics().horizontalAdvance(ellipsis), QTextLine::CursorOnCharacter);
        visibleWidth = line.cursorToX(firstHiddenCharacter);
    }

    painter.save();
    painter.setClipRect(QRectF(0, 0, visibleWidth, height()));
    painter.translate(0, top);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, textColor);
    document->documentLayout()->draw(&painter, context);
    painter.restore();

    if (overflows) {
        const qreal baseline = top + document->documentLayout()->blockBoundingRect(block).top() + line.y() + line.ascent();
        painter.setPen(textColor);
        painter.drawText(QPointF(visibleWidth, baseline), ellipsis);
    }
}

void ChannelTopicLine::mouseReleaseEvent(QMouseEvent *event)
{
    if (document && event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit clicked();
    QWidget::mouseReleaseEvent(event);
}

} // namespace UI
} // namespace Acheron
