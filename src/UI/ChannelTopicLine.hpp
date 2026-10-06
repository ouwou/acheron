#pragma once

#include <QWidget>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {

class MessageTextDocument;

class ChannelTopicLine : public QWidget
{
    Q_OBJECT
public:
    explicit ChannelTopicLine(Core::ImageManager *images, QWidget *parent = nullptr);

    void setTopicHtml(const QString &html, Core::Snowflake accountId);

    QSize sizeHint() const override;

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    Core::ImageManager *images;
    MessageTextDocument *document = nullptr;
};

} // namespace UI
} // namespace Acheron
