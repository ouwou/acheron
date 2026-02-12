#pragma once

#include <QStyledItemDelegate>

namespace Acheron {
namespace Core {
class ImageManager;
}
namespace UI {
class ChatDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChatDelegate(Core::ImageManager *imageManager, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), imageManager(imageManager)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    Core::ImageManager *imageManager;
};
} // namespace UI
} // namespace Acheron
