#pragma once

#include "UI/Dialogs/BasePopup.hpp"

#include "Core/ChannelPins.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>

class QLabel;
class QPushButton;
class QScrollArea;
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

    void configure(Core::ClientInstance *instance, Core::Snowflake channelId, const QString &channelName, bool canUnpin);

    static void confirmUnpin(QWidget *parent, Core::ChannelPins *pins, Core::Snowflake channelId, Core::Snowflake messageId);

signals:
    void messageActivated(Core::Snowflake channelId, Core::Snowflake messageId);
    void linkActivated(const QString &url);

private:
    [[nodiscard]] Core::ChannelPins *pins() const;
    [[nodiscard]] const Core::ChannelPins::Channel *knownPins() const;
    [[nodiscard]] int bodyWidth() const;

    void onPinsChanged(Core::Snowflake changedChannelId);
    void onLoadStateChanged(Core::Snowflake changedChannelId);
    void rebuildCards();
    void updateStatus();
    void loadMore();
    void loadMoreIfNearBottom();
    void addCard(const Core::ChannelPins::Pinned &pin);

    Core::ImageManager *images;
    QPointer<Core::ClientInstance> instance;
    Core::Snowflake channelId;
    bool isPrivate = false;
    bool canUnpin = false;
    QMetaObject::Connection pinsConnection;
    QMetaObject::Connection loadStateConnection;

    QLabel *titleLabel = nullptr;
    QScrollArea *scroll = nullptr;
    QVBoxLayout *cardsLayout = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *tipLabel = nullptr;
    QPushButton *loadMoreButton = nullptr;
};

} // namespace UI
} // namespace Acheron
