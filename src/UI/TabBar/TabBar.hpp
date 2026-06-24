#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {

struct TabEntry
{
    Core::Snowflake channelId;
    Core::Snowflake guildId;
    Core::Snowflake accountId;
    QString name;
    QUrl iconUrl;
    bool isDm = false;

    bool operator==(const TabEntry &o) const { return channelId == o.channelId; }
};

struct Tab
{
    QList<TabEntry> history;
    int historyIndex = 0;

    const TabEntry &current() const { return history[historyIndex]; }
    bool canGoBack() const { return historyIndex > 0; }
    bool canGoForward() const { return historyIndex < history.size() - 1; }
};

class TabBar : public QWidget
{
    Q_OBJECT
public:
    explicit TabBar(Core::ImageManager *imageManager, QWidget *parent = nullptr);

    void updateCurrentTab(const TabEntry &entry);
    void openNewTab(const TabEntry &entry);

    void navigateBack();
    void navigateForward();

    bool canNavigateBack() const;
    bool canNavigateForward() const;

    int tabCount() const { return tabs.size(); }
    const TabEntry &tabEntry(int index) const { return tabs[index].current(); }
    QString activeTabName() const { return tabs.isEmpty() ? QString() : tabs[currentTabIndex].current().name; }

    int activeTabIndex() const { return currentTabIndex; }
    QList<TabEntry> tabEntries() const;
    void restoreTabs(const QList<TabEntry> &entries, int activeIndex);

    void updateChannelReadState(Core::Snowflake channelId, bool unread, int mentionCount);

signals:
    void tabChanged(const TabEntry &entry);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    struct TabRect
    {
        QRect tab;
        QRect closeBtn;
    };

    QList<TabRect> computeTabRects() const;
    int tabAtPos(const QPoint &pos) const;
    void closeTab(int index);
    void switchToTab(int index);
    void updateVisibility();

    struct ChannelReadInfo
    {
        bool unread = false;
        int mentionCount = 0;
    };

    Core::ImageManager *imageManager;

    QHash<Core::Snowflake, ChannelReadInfo> channelReadStates;
    QList<Tab> tabs;
    int currentTabIndex = 0;
    int hoveredTab = -1;
    int hoveredClose = -1;

    bool dragging = false;
    int dragSourceIndex = -1;
    QPoint dragStartPos;

    constexpr static int TabHeight = 28;
    constexpr static int TabMaxWidth = 180;
    constexpr static int TabMinWidth = 60;
    constexpr static int TabPadding = 8;
    constexpr static int CloseButtonSize = 14;
    constexpr static int TabSpacing = 1;
    constexpr static int IconSize = 16;
    constexpr static int IconSpacing = 5;
    constexpr static int BadgeHeight = 16;
    constexpr static int BadgeMinWidth = 16;
    constexpr static int BadgePadding = 5;
};

} // namespace UI
} // namespace Acheron
