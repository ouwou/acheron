#pragma once

#include <QStyledItemDelegate>

namespace Acheron {
namespace Core {
class ImageManager;
} // namespace Core
namespace UI {
class ChatModel;
class ChatView;
class EmojiTextObject;

class ChatDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChatDelegate(Core::ImageManager *imageManager, ChatView *view);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    bool paintBodyTextOnly(QPainter *painter, const QStyleOptionViewItem &option,
                           const QModelIndex &index, const ChatModel *chatModel, const ChatView *chatView,
                           const QRect &textRect, const QRect &damage) const;

    EmojiTextObject *emojiHandler;
};
} // namespace UI
} // namespace Acheron
