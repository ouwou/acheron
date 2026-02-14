#pragma once

#include <QStyledItemDelegate>

namespace Acheron {
namespace UI {

class MemberListDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit MemberListDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    void paintGroup(QPainter *painter, const QStyleOptionViewItem &option,
                    const QModelIndex &index) const;
    void paintMember(QPainter *painter, const QStyleOptionViewItem &option,
                     const QModelIndex &index) const;
    void paintPlaceholder(QPainter *painter, const QStyleOptionViewItem &option) const;
};

} // namespace UI
} // namespace Acheron
