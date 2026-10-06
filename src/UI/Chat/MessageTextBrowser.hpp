#pragma once

#include <QTextBrowser>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {

class MessageTextDocument;

class MessageTextBrowser : public QTextBrowser
{
    Q_OBJECT
public:
    MessageTextBrowser(Core::ImageManager *images, Core::Snowflake accountId, QWidget *parent = nullptr);

    void setMessageHtml(const QString &html, int textWidth);
    void fitHeightToDocument();

signals:
    void linkActivated(const QString &url);

private:
    MessageTextDocument *messageDocument;
};

} // namespace UI
} // namespace Acheron
