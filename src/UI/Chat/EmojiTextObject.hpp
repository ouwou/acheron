#pragma once

#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QObject>
#include <QUrl>

#include "Core/Snowflake.hpp"

class QTextDocument;

namespace Acheron {
namespace Core {
class ImageManager;
} // namespace Core

namespace UI {

class EmojiAnimator;

class EmojiTextObject : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    EmojiTextObject(Core::ImageManager *imageManager, EmojiAnimator *animator, QObject *parent = nullptr);

    void install(QTextDocument &doc);
    void setAccountId(Core::Snowflake id) { accountId = id; }

    QSizeF intrinsicSize(QTextDocument *doc, int posInDocument, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc, int posInDocument, const QTextFormat &format) override;

private:
    struct ImageSource
    {
        QUrl url;
        QUrl still;
        bool isEmoji = false;
        bool isAnimatedEmoji = false;
    };

    const ImageSource &source(const QString &name);

    Core::ImageManager *imageManager;
    EmojiAnimator *animator;
    Core::Snowflake accountId;
    QHash<QString, ImageSource> sources;
};

} // namespace UI
} // namespace Acheron
