#pragma once

#include "UI/Dialogs/BasePopup.hpp"

#include "Core/Snowflake.hpp"
#include "Discord/Client.hpp"

#include <QDateTime>
#include <QPointer>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace Acheron {
namespace Core {
class ClientInstance;
class ImageManager;
}
namespace UI {

class PinnedMessagesPopup : public BasePopup
{
    Q_OBJECT
public:
    explicit PinnedMessagesPopup(Core::ImageManager *images, QWidget *parent = nullptr);

    void configure(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &channelName);

signals:
    void messageActivated(Core::Snowflake channelId, Core::Snowflake messageId);
    void linkActivated(const QString &url);

private:
    void reload();
    void loadPage();
    void clearItems();
    void addNotice(const QString &text);
    void addPin(const Discord::Client::MessagePin &pin);

    Core::ImageManager *images;
    QPointer<Core::ClientInstance> instance;
    Core::Snowflake channelId;

    QLabel *titleLabel = nullptr;
    QVBoxLayout *itemsLayout = nullptr;
    QPushButton *loadMoreButton = nullptr;

    QDateTime oldestPinnedAt;
    int loadGeneration = 0;
};

} // namespace UI
} // namespace Acheron
