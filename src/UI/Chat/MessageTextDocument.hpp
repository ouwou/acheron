#pragma once

#include <QTextDocument>

#include "Core/Snowflake.hpp"
#include "UI/Chat/EmojiTextObject.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {

class MessageTextDocument : public QTextDocument
{
    Q_OBJECT
public:
    MessageTextDocument(Core::ImageManager *images, Core::Snowflake accountId, QObject *parent = nullptr);

    void setMessageHtml(const QString &html, const QFont &font, int textWidth);

signals:
    void imageLoaded();

private:
    EmojiTextObject emoji;
};

} // namespace UI
} // namespace Acheron
