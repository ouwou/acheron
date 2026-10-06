#include "MentionJumpIndicator.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QEvent>
#include <QPainter>
#include <QScrollBar>

namespace Acheron {
namespace UI {

namespace {

constexpr int PillHeight = 24;
constexpr int PillMargin = 8;
constexpr int PillFontPx = 12;
constexpr int PressedDarkenPercent = 115;
constexpr int OfficialRecalculateDelayMs = 200;

class JumpPill : public QAbstractButton
{
public:
    explicit JumpPill(QWidget *parent) : QAbstractButton(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        QFont pillFont = font();
        pillFont.setBold(true);
        pillFont.setPixelSize(PillFontPx);
        setFont(pillFont);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QColor fill = palette().color(QPalette::Highlight);
        painter.setBrush(isDown() ? fill.darker(PressedDarkenPercent) : fill);
        painter.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);
        painter.setPen(palette().color(QPalette::HighlightedText));
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

} // namespace

MentionJumpIndicator::MentionJumpIndicator(QAbstractItemView *view, RowStep rowAbove, RowStep rowBelow, MentionTest hasMentions)
    : QObject(view), view(view), rowAbove(std::move(rowAbove)), rowBelow(std::move(rowBelow)), hasMentions(std::move(hasMentions)), abovePill(new JumpPill(view)), belowPill(new JumpPill(view))
{
    for (QAbstractButton *pill : { abovePill.data(), belowPill.data() })
        pill->setText(tr("NEW"));
    connect(abovePill, &QAbstractButton::clicked, this, [this]() { jumpTo(aboveTarget); });
    connect(belowPill, &QAbstractButton::clicked, this, [this]() { jumpTo(belowTarget); });

    updateTimer.setSingleShot(true);
    updateTimer.setInterval(OfficialRecalculateDelayMs);
    connect(&updateTimer, &QTimer::timeout, this, &MentionJumpIndicator::updateTargets);

    const QAbstractItemModel *model = view->model();
    connect(model, &QAbstractItemModel::dataChanged, this, &MentionJumpIndicator::scheduleUpdate);
    connect(model, &QAbstractItemModel::rowsInserted, this, &MentionJumpIndicator::scheduleUpdate);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &MentionJumpIndicator::scheduleUpdate);
    connect(model, &QAbstractItemModel::rowsMoved, this, &MentionJumpIndicator::scheduleUpdate);
    connect(model, &QAbstractItemModel::layoutChanged, this, &MentionJumpIndicator::scheduleUpdate);
    connect(model, &QAbstractItemModel::modelReset, this, &MentionJumpIndicator::scheduleUpdate);
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, &MentionJumpIndicator::scheduleUpdate);
    connect(view->verticalScrollBar(), &QScrollBar::rangeChanged, this, &MentionJumpIndicator::scheduleUpdate);

    view->viewport()->installEventFilter(this);
    layoutPills();
    scheduleUpdate();
}

MentionJumpIndicator::~MentionJumpIndicator()
{
    delete abovePill;
    delete belowPill;
}

void MentionJumpIndicator::setEnabled(bool nowEnabled)
{
    enabled = nowEnabled;
    updateTargets();
}

bool MentionJumpIndicator::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show) {
        layoutPills();
        scheduleUpdate();
    }
    return QObject::eventFilter(watched, event);
}

void MentionJumpIndicator::scheduleUpdate()
{
    if (!updateTimer.isActive())
        updateTimer.start();
}

void MentionJumpIndicator::updateTargets()
{
    showTarget(abovePill, aboveTarget, enabled ? farthestMentionAbove() : QModelIndex());
    showTarget(belowPill, belowTarget, enabled ? farthestMentionBelow() : QModelIndex());
}

void MentionJumpIndicator::showTarget(QAbstractButton *pill, QPersistentModelIndex &target, const QModelIndex &found)
{
    target = found;
    pill->setVisible(found.isValid());
    if (found.isValid())
        pill->raise();
}

void MentionJumpIndicator::layoutPills()
{
    const QRect area = view->viewport()->geometry().adjusted(PillMargin, PillMargin, -PillMargin, -PillMargin);
    abovePill->setGeometry(area.left(), area.top(), area.width(), PillHeight);
    belowPill->setGeometry(area.left(), area.bottom() - PillHeight + 1, area.width(), PillHeight);
}

void MentionJumpIndicator::jumpTo(const QPersistentModelIndex &target)
{
    if (target.isValid())
        view->scrollTo(target, QAbstractItemView::EnsureVisible);
}

QModelIndex MentionJumpIndicator::farthestMentionAbove() const
{
    const QModelIndex topRow = view->indexAt(QPoint(view->viewport()->width() / 2, 0));
    if (!topRow.isValid())
        return {};
    const bool cutOff = view->visualRect(topRow).top() < 0;
    return farthestMentionFrom(cutOff ? topRow : rowAbove(topRow), rowAbove);
}

QModelIndex MentionJumpIndicator::farthestMentionBelow() const
{
    const int lastVisibleY = view->viewport()->height() - 1;
    const QModelIndex bottomRow = view->indexAt(QPoint(view->viewport()->width() / 2, lastVisibleY));
    if (!bottomRow.isValid())
        return {};
    const bool cutOff = view->visualRect(bottomRow).bottom() > lastVisibleY;
    return farthestMentionFrom(cutOff ? bottomRow : rowBelow(bottomRow), rowBelow);
}

QModelIndex MentionJumpIndicator::farthestMentionFrom(QModelIndex row, const RowStep &step) const
{
    QModelIndex farthest;
    for (; row.isValid(); row = step(row)) {
        if (hasMentions(row))
            farthest = row;
    }
    return farthest;
}

} // namespace UI
} // namespace Acheron
