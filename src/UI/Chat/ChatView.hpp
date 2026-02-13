#pragma once

#include <QtWidgets>
#include <QClipboard>
#include <QGuiApplication>

#include "ChatLayout.hpp"
#include "ChatModel.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

class ImageViewer;
struct ChatCursor
{
    int row = -1;
    int index = -1;

    bool isValid() const { return row >= 0 && index >= 0; }

    bool operator==(const ChatCursor &other) const
    {
        return row == other.row && index == other.index;
    }
    bool operator!=(const ChatCursor &other) const { return !(*this == other); }
    bool operator<(const ChatCursor &other) const
    {
        if (row != other.row)
            return row < other.row;
        return index < other.index;
    }
};

class ChatView : public QListView
{
    Q_OBJECT
public:
    ChatView(QWidget *parent = nullptr);

    int hoveredRowAtPaint() const { return hoveredRow; }
    int hoveredCharIndexAtPaint() const { return hoveredChar; }
    int editingRow() const { return currentEditingIndex.isValid() ? currentEditingIndex.row() : -1; }

    static constexpr int InlineEditMinHeight = 60;

    bool hasTextSelection() const;

    ChatCursor selectionStart() const;
    ChatCursor selectionEnd() const;

    void setModel(QAbstractItemModel *model) override;
    void setCurrentUserId(Core::Snowflake userId);
    void setCanPinMessages(bool canPin);
    void setCanManageMessages(bool canManage);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void clearSelection();
    void leaveEvent(QEvent *event) override;

signals:
    void historyRequested();
    void editMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId, const QString &currentContent);
    void deleteMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void pinMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void replyToMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void addReactionRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void toggleReactionClicked(Core::Snowflake channelId, Core::Snowflake messageId,
                               const QString &emoji, bool currentlyReacted, bool isBurst);

public slots:
    void onHistoryRequestFinished();

private slots:
    void onScrollBarValueChanged(int value);
    void onRowsAboutToBeInserted(const QModelIndex &parent, int start, int end);
    void onRowsInserted(const QModelIndex &parent, int start, int end);

private:
    void copySelectedText();
    void copyMessageContent(const QModelIndex &index);
    void startInlineEdit(const QModelIndex &index);
    void commitInlineEdit();
    void cancelInlineEdit();

    QTextEdit *inlineEditWidget = nullptr;
    Core::Snowflake currentEditingMessageId = Core::Snowflake::Invalid;
    QModelIndex currentEditingIndex;

    int hoveredRow;
    int hoveredChar;

    ChatCursor selectionAnchor;
    ChatCursor selectionHead;

    bool isFetchingTop = false;
    bool isFetchingBottom = false;

    QPersistentModelIndex anchorIndex;
    int anchorDistanceFromBottom = 0;

    bool atBottom = false;

    Core::Snowflake currentUserId = Core::Snowflake::Invalid;
    bool canPinMessages = false;
    bool canManageMessages = false;
};
} // namespace UI
} // namespace Acheron
