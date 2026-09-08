#include "UI/Chat/EmojiTextObject.hpp"

#include <QPainter>
#include <QTextDocument>
#include <QTextImageFormat>

#include "Core/ImageManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/Chat/EmojiAnimator.hpp"

namespace Acheron {
namespace UI {

namespace {

QPixmap resourcePixmap(QTextDocument *doc, const QUrl &url)
{
    const QVariant resource = doc->resource(QTextDocument::ImageResource, url);
    if (resource.canConvert<QPixmap>())
        return qvariant_cast<QPixmap>(resource);
    if (resource.canConvert<QImage>())
        return QPixmap::fromImage(qvariant_cast<QImage>(resource));
    return {};
}

} // namespace

EmojiTextObject::EmojiTextObject(Core::ImageManager *imageManager, EmojiAnimator *animator, QObject *parent)
    : QObject(parent), imageManager(imageManager), animator(animator)
{
}

void EmojiTextObject::install(QTextDocument &doc)
{
    doc.documentLayout()->registerHandler(QTextFormat::ImageObject, this);
}

const EmojiTextObject::ImageSource &EmojiTextObject::source(const QString &name)
{
    auto it = sources.find(name);
    if (it != sources.end())
        return *it;

    ImageSource src;
    src.url = QUrl(name);
    src.isEmoji = Discord::Cdn::isEmojiUrl(src.url);
    src.isAnimatedEmoji = src.isEmoji && Discord::Cdn::isAnimatedEmojiUrl(src.url);
    src.still = src.isAnimatedEmoji ? Discord::Cdn::stillEmojiUrl(src.url) : src.url;
    return *sources.insert(name, src);
}

QSizeF EmojiTextObject::intrinsicSize(QTextDocument *doc, int, const QTextFormat &format)
{
    const QTextImageFormat imageFormat = format.toImageFormat();
    if (imageFormat.hasProperty(QTextFormat::ImageWidth) && imageFormat.hasProperty(QTextFormat::ImageHeight))
        return QSizeF(imageFormat.width(), imageFormat.height());

    const QPixmap pixmap = resourcePixmap(doc, source(imageFormat.name()).url);
    if (pixmap.isNull())
        return QSizeF(0, 0);
    return QSizeF(pixmap.size()) / pixmap.devicePixelRatio();
}

void EmojiTextObject::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int, const QTextFormat &format)
{
    const QTextImageFormat imageFormat = format.toImageFormat();
    const ImageSource &src = source(imageFormat.name());
    const QRect target = rect.toAlignedRect();

    QPixmap pixmap;
    if (src.isEmoji) {
        const QSize declaredSize(qRound(imageFormat.width()), qRound(imageFormat.height()));
        const bool animate = src.isAnimatedEmoji && animator->isEnabled();
        if (animate)
            pixmap = animator->frame(src.url, declaredSize, accountId, painter->transform().mapRect(target));
        if (pixmap.isNull())
            pixmap = imageManager->get(animate ? src.url : src.still, declaredSize, accountId);
    } else {
        pixmap = resourcePixmap(doc, src.url);
    }

    if (!pixmap.isNull())
        painter->drawPixmap(target, pixmap);
}

} // namespace UI
} // namespace Acheron
