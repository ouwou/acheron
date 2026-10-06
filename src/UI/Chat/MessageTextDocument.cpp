#include "MessageTextDocument.hpp"

#include "Core/ImageManager.hpp"
#include "UI/Chat/ChatLayout.hpp"

namespace Acheron {
namespace UI {

MessageTextDocument::MessageTextDocument(Core::ImageManager *images, Core::Snowflake accountId, QObject *parent)
    : QTextDocument(parent), emoji(images, nullptr, this)
{
    emoji.setAccountId(accountId);
    connect(images, &Core::ImageManager::imageFetched, this, &MessageTextDocument::imageLoaded);
}

void MessageTextDocument::setMessageHtml(const QString &html, const QFont &font, int textWidth)
{
    ChatLayout::setupDocument(*this, html, font, textWidth);
    emoji.install(*this);
}

} // namespace UI
} // namespace Acheron
