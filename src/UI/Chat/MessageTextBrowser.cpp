#include "MessageTextBrowser.hpp"

#include <QAbstractTextDocumentLayout>

#include <cmath>

#include "UI/Chat/MessageTextDocument.hpp"

namespace Acheron {
namespace UI {

MessageTextBrowser::MessageTextBrowser(Core::ImageManager *images, Core::Snowflake accountId, QWidget *parent)
    : QTextBrowser(parent), messageDocument(new MessageTextDocument(images, accountId, this))
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setOpenLinks(false);
    setDocument(messageDocument);

    connect(messageDocument, &MessageTextDocument::imageLoaded, viewport(), qOverload<>(&QWidget::update));
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { emit linkActivated(QString::fromUtf8(url.toEncoded())); });
}

void MessageTextBrowser::setMessageHtml(const QString &html, int textWidth)
{
    messageDocument->setMessageHtml(html, font(), textWidth);
}

void MessageTextBrowser::fitHeightToDocument()
{
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(messageDocument->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF &size) { setFixedHeight(int(std::ceil(size.height()))); });
}

} // namespace UI
} // namespace Acheron
