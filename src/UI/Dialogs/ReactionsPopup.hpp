#pragma once

#include <QColor>
#include <QIcon>
#include <QList>

#include <optional>

#include "BasePopup.hpp"
#include "Core/MessageReactors.hpp"
#include "Core/Snowflake.hpp"

class QListView;
class QListWidget;
class QModelIndex;
class QPushButton;

namespace Acheron {
namespace Core {
class ClientInstance;
class ImageManager;
} // namespace Core
namespace UI {

class PaintedImages;
class ReactorListModel;

class ReactionsPopup : public BasePopup
{
    Q_OBJECT
public:
    enum class RemovableReactors {
        None,
        OwnOnly,
        Anyone,
    };

    ReactionsPopup(Core::ImageManager *images, Core::ClientInstance *instance, Core::Snowflake channelId, Core::Snowflake guildId, Core::Snowflake messageId, const std::optional<Core::ReactionRef> &selected, RemovableReactors removable, QWidget *parent = nullptr);

signals:
    void userActivated(Core::Snowflake userId);

private slots:
    void onReactionsChanged(Core::Snowflake changedMessageId);
    void onReactorsChanged(Core::Snowflake changedMessageId);
    void onMessageDeleted(Core::Snowflake deletedChannelId, Core::Snowflake deletedMessageId);
    void onTabSelected(int row);
    void onReactorClicked(const QModelIndex &index);
    void onReactorsScrolled();
    void refreshTabIcons();

private:
    struct ReactionState
    {
        int normalCount = 0;
        int superCount = 0;
        bool me = false;
        bool meSuper = false;

        bool operator==(const ReactionState &) const = default;
    };
    struct Tab
    {
        Core::ReactionRef reaction;
        int count = 0;
        QColor superTint;
        ReactionState wholeReaction;
    };

    void syncWithMessage();
    void showSelected();
    [[nodiscard]] int tabRowOf(const std::optional<Core::ReactionRef> &reaction) const;
    void showKnownReactors();
    void restartPaging();
    void loadPage();
    [[nodiscard]] bool hasMoreReactors() const;
    [[nodiscard]] bool canRemove(Core::Snowflake reactorId) const;
    [[nodiscard]] QIcon tabIcon(const Discord::Emoji &emoji);

    Core::ClientInstance *instance;
    Core::Snowflake channelId;
    Core::Snowflake guildId;
    Core::Snowflake messageId;
    RemovableReactors removable;

    QListWidget *tabList = nullptr;
    QListView *reactorView = nullptr;
    ReactorListModel *reactorModel = nullptr;
    QPushButton *retryPageButton = nullptr;
    PaintedImages *tabIcons = nullptr;

    QList<Tab> tabs;
    std::optional<Core::ReactionRef> selected;
    std::optional<Core::ReactionRef> shown;
    ReactionState shownState;

    std::optional<Core::Snowflake> lastPageEnd;
    bool awaitingPage = false;
    int latestPageRequest = 0;
};

} // namespace UI
} // namespace Acheron
