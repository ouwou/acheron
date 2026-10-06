#pragma once

#include "BasePopup.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {

class ChannelTopicPopup : public BasePopup
{
    Q_OBJECT
public:
    ChannelTopicPopup(const QString &channelName, const QString &topicHtml, Core::ImageManager *images, Core::Snowflake accountId, QWidget *parent = nullptr);

signals:
    void linkActivated(const QString &url);
};

} // namespace UI
} // namespace Acheron
