#pragma once

#include <QModelIndex>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTimer>

#include <functional>

class QAbstractButton;
class QAbstractItemView;

namespace Acheron {
namespace UI {

class MentionJumpIndicator : public QObject
{
    Q_OBJECT
public:
    using RowStep = std::function<QModelIndex(const QModelIndex &)>;
    using MentionTest = std::function<bool(const QModelIndex &)>;

    MentionJumpIndicator(QAbstractItemView *view, RowStep rowAbove, RowStep rowBelow, MentionTest hasMentions);
    ~MentionJumpIndicator() override;

    void setEnabled(bool enabled);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scheduleUpdate();
    void updateTargets();
    void layoutPills();
    void showTarget(QAbstractButton *pill, QPersistentModelIndex &target, const QModelIndex &found);
    void jumpTo(const QPersistentModelIndex &target);

    [[nodiscard]] QModelIndex farthestMentionAbove() const;
    [[nodiscard]] QModelIndex farthestMentionBelow() const;
    [[nodiscard]] QModelIndex farthestMentionFrom(QModelIndex row, const RowStep &step) const;

    QAbstractItemView *view;
    RowStep rowAbove;
    RowStep rowBelow;
    MentionTest hasMentions;

    QPointer<QAbstractButton> abovePill;
    QPointer<QAbstractButton> belowPill;
    QPersistentModelIndex aboveTarget;
    QPersistentModelIndex belowTarget;

    QTimer updateTimer;
    bool enabled = true;
};

} // namespace UI
} // namespace Acheron
