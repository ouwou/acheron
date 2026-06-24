#include "TabBar.hpp"
#include "Core/ImageManager.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QApplication>

namespace Acheron {
namespace UI {

TabBar::TabBar(Core::ImageManager *imageManager, QWidget *parent)
    : QWidget(parent), imageManager(imageManager)
{
    setMouseTracking(true);
    setFixedHeight(TabHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // one empty tab
    Tab initial;
    initial.history.append({ Core::Snowflake::Invalid, Core::Snowflake::Invalid,
                             Core::Snowflake::Invalid, QString() });
    tabs.append(initial);
    currentTabIndex = 0;

    connect(imageManager, &Core::ImageManager::imageFetched, this, [this](const QUrl &url, const QSize &, const QPixmap &) {
        for (const auto &tab : tabs) {
            if (tab.current().iconUrl == url) {
                update();
                return;
            }
        }
    });

    updateVisibility();
}

void TabBar::updateCurrentTab(const TabEntry &entry)
{
    if (!entry.channelId.isValid())
        return;

    Tab &tab = tabs[currentTabIndex];

    // dont push if already here
    if (!tab.history.isEmpty() && tab.current().channelId == entry.channelId)
        return;

    // if the tab was at the initial empty state, replace it
    if (tab.history.size() == 1 && !tab.history[0].channelId.isValid()) {
        tab.history[0] = entry;
        tab.historyIndex = 0;
        update();
        return;
    }

    // truncate forward history
    while (tab.history.size() > tab.historyIndex + 1)
        tab.history.removeLast();

    tab.history.append(entry);
    tab.historyIndex = tab.history.size() - 1;

    constexpr static int MaxHistory = 50;
    if (tab.history.size() > MaxHistory) {
        int excess = tab.history.size() - MaxHistory;
        tab.history.remove(0, excess);
        tab.historyIndex -= excess;
    }

    update();
}

void TabBar::openNewTab(const TabEntry &entry)
{
    Tab newTab;
    newTab.history.append(entry);
    newTab.historyIndex = 0;

    int insertAt = currentTabIndex + 1;
    tabs.insert(insertAt, newTab);
    currentTabIndex = insertAt;

    updateVisibility();
    update();
    emit tabChanged(entry);
}

QList<TabEntry> TabBar::tabEntries() const
{
    QList<TabEntry> out;
    out.reserve(tabs.size());
    for (const auto &tab : tabs)
        out.append(tab.current());
    return out;
}

void TabBar::restoreTabs(const QList<TabEntry> &entries, int activeIndex)
{
    if (entries.isEmpty())
        return;

    tabs.clear();
    for (const auto &entry : entries) {
        Tab tab;
        tab.history.append(entry);
        tab.historyIndex = 0;
        tabs.append(tab);
    }

    currentTabIndex = qBound(0, activeIndex, tabs.size() - 1);

    updateVisibility();
    update();
}

void TabBar::navigateBack()
{
    Tab &tab = tabs[currentTabIndex];
    if (!tab.canGoBack())
        return;

    tab.historyIndex--;
    update();
    emit tabChanged(tab.current());
}

void TabBar::navigateForward()
{
    Tab &tab = tabs[currentTabIndex];
    if (!tab.canGoForward())
        return;

    tab.historyIndex++;
    update();
    emit tabChanged(tab.current());
}

bool TabBar::canNavigateBack() const
{
    return tabs[currentTabIndex].canGoBack();
}

bool TabBar::canNavigateForward() const
{
    return tabs[currentTabIndex].canGoForward();
}

void TabBar::updateChannelReadState(Core::Snowflake channelId, bool unread, int mentionCount)
{
    if (!channelId.isValid())
        return;

    auto it = channelReadStates.find(channelId);
    if (it != channelReadStates.end() && it->unread == unread && it->mentionCount == mentionCount)
        return;

    channelReadStates[channelId] = { unread, mentionCount };
    if (isVisible())
        update();
}

QList<TabBar::TabRect> TabBar::computeTabRects() const
{
    QList<TabRect> rects;
    QFontMetrics fm(font());

    int x = 0;
    for (int i = 0; i < tabs.size(); ++i) {
        const Tab &tab = tabs[i];
        QString label = tab.current().name;
        if (label.isEmpty())
            label = "(no channel)";

        int textW = fm.horizontalAdvance(label);
        int tabW = TabPadding + IconSize + IconSpacing + textW + TabPadding;

        auto readIt = channelReadStates.constFind(tab.current().channelId);
        if (readIt != channelReadStates.constEnd() && readIt->mentionCount > 0) {
            QString badgeText = QString::number(readIt->mentionCount);
            int badgeTextW = fm.horizontalAdvance(badgeText);
            int badgeW = qMax(BadgeMinWidth, badgeTextW + BadgePadding * 2);
            tabW += badgeW + 4;
        }

        if (tabs.size() > 1)
            tabW += CloseButtonSize + 4;

        tabW = qBound(TabMinWidth, tabW, TabMaxWidth);

        QRect tabRect(x, 0, tabW, TabHeight);

        QRect closeRect;
        if (tabs.size() > 1) {
            int closeX = x + tabW - TabPadding - CloseButtonSize;
            int closeY = (TabHeight - CloseButtonSize) / 2;
            closeRect = QRect(closeX, closeY, CloseButtonSize, CloseButtonSize);
        }

        rects.append({ tabRect, closeRect });
        x += tabW + TabSpacing;
    }

    return rects;
}

int TabBar::tabAtPos(const QPoint &pos) const
{
    auto rects = computeTabRects();
    for (int i = 0; i < rects.size(); ++i) {
        if (rects[i].tab.contains(pos))
            return i;
    }
    return -1;
}

void TabBar::closeTab(int index)
{
    if (tabs.size() <= 1)
        return;

    bool wasActive = (index == currentTabIndex);

    tabs.removeAt(index);

    if (currentTabIndex >= tabs.size())
        currentTabIndex = tabs.size() - 1;
    else if (index < currentTabIndex)
        currentTabIndex--;
    // if the user closed the current tab, currentTabIndex now points to the
    // tab that slid into this position (or the one before if it was the last)

    updateVisibility();
    update();

    if (wasActive)
        emit tabChanged(tabs[currentTabIndex].current());
}

void TabBar::switchToTab(int index)
{
    if (index == currentTabIndex || index < 0 || index >= tabs.size())
        return;

    currentTabIndex = index;
    update();
    emit tabChanged(tabs[currentTabIndex].current());
}

void TabBar::updateVisibility()
{
    setVisible(tabs.size() > 1);
}

void TabBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QPalette &pal = palette();
    QColor windowBg = pal.color(QPalette::Window);
    QColor altBase = pal.color(QPalette::AlternateBase);
    QColor highlight = pal.color(QPalette::Highlight);
    QColor textColor = pal.color(QPalette::Text);
    QColor dimText = textColor;
    dimText.setAlpha(140);

    p.fillRect(rect(), windowBg);

    p.setPen(QPen(altBase, 1));
    p.drawLine(0, height() - 1, width(), height() - 1);

    auto rects = computeTabRects();
    QFontMetrics fm(font());

    for (int i = 0; i < rects.size(); ++i) {
        const TabRect &tr = rects[i];
        bool isActive = (i == currentTabIndex);
        bool isHovered = (i == hoveredTab);

        QColor tabBg;
        if (isActive)
            tabBg = altBase;
        else if (isHovered)
            tabBg = QColor((windowBg.red() + altBase.red()) / 2,
                           (windowBg.green() + altBase.green()) / 2,
                           (windowBg.blue() + altBase.blue()) / 2);
        else
            tabBg = windowBg;

        QRect tabR = tr.tab.adjusted(0, 2, 0, 0);
        p.setPen(Qt::NoPen);
        p.setBrush(tabBg);
        p.drawRoundedRect(tabR.adjusted(0, 0, 0, 4), 4, 4);

        // accent bar for active tab
        if (isActive) {
            p.setPen(Qt::NoPen);
            p.setBrush(highlight);
            p.drawRoundedRect(QRect(tabR.left() + 8, tabR.top(), tabR.width() - 16, 2), 1, 1);
        }

        const TabEntry &entry = tabs[i].current();
        int iconX = tr.tab.left() + TabPadding;
        int iconY = tr.tab.top() + (tr.tab.height() - IconSize) / 2;
        QRect iconRect(iconX, iconY, IconSize, IconSize);

        if (entry.isDm) {
            p.save();
            QFont iconFont = font();
            iconFont.setPixelSize(IconSize - 2);
            iconFont.setBold(true);
            p.setFont(iconFont);
            p.setPen(isActive ? highlight : dimText);
            p.drawText(iconRect, Qt::AlignCenter, QStringLiteral("@"));
            p.restore();
        } else if (!entry.iconUrl.isEmpty()) {
            QPixmap icon = imageManager->get(entry.iconUrl, QSize(IconSize * 2, IconSize * 2), Core::PinGroup::ChannelList);
            if (!icon.isNull()) {
                p.save();
                QPainterPath clipPath;
                clipPath.addRoundedRect(iconRect, 3, 3);
                p.setClipPath(clipPath);
                p.drawPixmap(iconRect, icon);
                p.restore();
            }
        }

        auto readIt = channelReadStates.constFind(entry.channelId);
        bool isUnread = !isActive && readIt != channelReadStates.constEnd() && readIt->unread;
        int mentions = (readIt != channelReadStates.constEnd()) ? readIt->mentionCount : 0;

        int badgeW = 0;
        QString badgeText;
        if (mentions > 0) {
            badgeText = QString::number(mentions);
            int badgeTextW = fm.horizontalAdvance(badgeText);
            badgeW = qMax(BadgeMinWidth, badgeTextW + BadgePadding * 2);
        }

        QString label = entry.name;
        if (label.isEmpty())
            label = "(no channel)";

        int textLeft = iconX + IconSize + IconSpacing;
        int textRight = tr.tab.right() - TabPadding;
        if (tabs.size() > 1)
            textRight = tr.closeBtn.left() - 4;
        if (badgeW > 0)
            textRight -= badgeW + 4;

        int availW = textRight - textLeft;
        QString elidedLabel = fm.elidedText(label, Qt::ElideRight, availW);

        QColor labelColor = isActive ? textColor : (isUnread ? highlight : dimText);
        if (isUnread && !isActive) {
            p.save();
            QFont boldFont = font();
            boldFont.setBold(true);
            p.setFont(boldFont);
            p.setPen(labelColor);
            QFontMetrics bfm(boldFont);
            QString boldElided = bfm.elidedText(label, Qt::ElideRight, availW);
            p.drawText(QRect(textLeft, tr.tab.top(), availW, tr.tab.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, boldElided);
            p.restore();
        } else {
            p.setPen(labelColor);
            p.drawText(QRect(textLeft, tr.tab.top(), availW, tr.tab.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, elidedLabel);
        }

        if (badgeW > 0) {
            int badgeX = textRight + 4;
            int badgeY = tr.tab.top() + (tr.tab.height() - BadgeHeight) / 2;
            QRect badgeRect(badgeX, badgeY, badgeW, BadgeHeight);

            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xED, 0x42, 0x45)); // Discord red
            p.drawRoundedRect(badgeRect, BadgeHeight / 2, BadgeHeight / 2);

            p.save();
            QFont badgeFont = font();
            badgeFont.setPixelSize(10);
            badgeFont.setBold(true);
            p.setFont(badgeFont);
            p.setPen(Qt::white);
            p.drawText(badgeRect, Qt::AlignCenter, badgeText);
            p.restore();
        }

        if (isUnread && mentions == 0) {
            int dotSize = 4;
            int dotX = tr.tab.left() + 3;
            int dotY = tr.tab.top() + (tr.tab.height() - dotSize) / 2;
            p.setPen(Qt::NoPen);
            p.setBrush(highlight);
            p.drawEllipse(dotX, dotY, dotSize, dotSize);
        }

        if (tabs.size() > 1 && (isActive || isHovered)) {
            bool closeHovered = (i == hoveredClose);
            QRect cr = tr.closeBtn;

            if (closeHovered) {
                p.setPen(Qt::NoPen);
                QColor closeBg = altBase.lighter(140);
                p.setBrush(closeBg);
                p.drawRoundedRect(cr, 3, 3);
            }

            p.setPen(QPen(closeHovered ? textColor : dimText, 1.5));
            int m = 4;
            p.drawLine(cr.left() + m, cr.top() + m, cr.right() - m, cr.bottom() - m);
            p.drawLine(cr.right() - m, cr.top() + m, cr.left() + m, cr.bottom() - m);
        }
    }
}

void TabBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        int idx = tabAtPos(event->pos());
        if (idx >= 0) {
            closeTab(idx);
            return;
        }
    }

    if (event->button() == Qt::LeftButton) {
        auto rects = computeTabRects();
        int idx = tabAtPos(event->pos());

        // check close button first
        if (idx >= 0 && tabs.size() > 1 && rects[idx].closeBtn.contains(event->pos())) {
            closeTab(idx);
            return;
        }

        if (idx >= 0) {
            dragSourceIndex = idx;
            dragStartPos = event->pos();
            dragging = false;
            switchToTab(idx);
        }
    }

    event->accept();
}

void TabBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragging = false;
        dragSourceIndex = -1;
    }
    event->accept();
}

void TabBar::mouseMoveEvent(QMouseEvent *event)
{
    auto rects = computeTabRects();
    int oldHovered = hoveredTab;
    int oldHoveredClose = hoveredClose;

    hoveredTab = tabAtPos(event->pos());
    hoveredClose = -1;

    if (hoveredTab >= 0 && tabs.size() > 1 && rects[hoveredTab].closeBtn.contains(event->pos()))
        hoveredClose = hoveredTab;

    if (hoveredTab != oldHovered || hoveredClose != oldHoveredClose)
        update();

    if (dragSourceIndex >= 0 && (event->buttons() & Qt::LeftButton)) {
        if (!dragging) {
            if ((event->pos() - dragStartPos).manhattanLength() >= QApplication::startDragDistance())
                dragging = true;
        }

        if (dragging) {
            int targetIdx = tabAtPos(event->pos());
            if (targetIdx >= 0 && targetIdx != dragSourceIndex) {
                tabs.move(dragSourceIndex, targetIdx);
                if (currentTabIndex == dragSourceIndex)
                    currentTabIndex = targetIdx;
                else if (dragSourceIndex < currentTabIndex && targetIdx >= currentTabIndex)
                    currentTabIndex--;
                else if (dragSourceIndex > currentTabIndex && targetIdx <= currentTabIndex)
                    currentTabIndex++;
                dragSourceIndex = targetIdx;
                update();
            }
        }
    }

    event->accept();
}

void TabBar::leaveEvent(QEvent *)
{
    if (hoveredTab != -1 || hoveredClose != -1) {
        hoveredTab = -1;
        hoveredClose = -1;
        update();
    }
}

QSize TabBar::sizeHint() const
{
    return QSize(400, TabHeight);
}

QSize TabBar::minimumSizeHint() const
{
    return QSize(100, TabHeight);
}

} // namespace UI
} // namespace Acheron
