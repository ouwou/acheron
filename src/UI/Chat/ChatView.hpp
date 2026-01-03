#pragma once

#include <QtWidgets>

#include "ChatLayout.hpp"

namespace Acheron {
namespace UI {
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

    bool hasTextSelection() const;

    ChatCursor selectionStart() const;
    ChatCursor selectionEnd() const;

    void setModel(QAbstractItemModel *model) override;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void clearSelection();
    void leaveEvent(QEvent *event) override;

signals:
    void historyRequested();

public slots:
    void onHistoryRequestFinished();

private slots:
    void onScrollBarValueChanged(int value);
    void onRowsAboutToBeInserted(const QModelIndex &parent, int start, int end);
    void onRowsInserted(const QModelIndex &parent, int start, int end);

private:
    int hoveredRow;
    int hoveredChar;

    ChatCursor selectionAnchor;
    ChatCursor selectionHead;

    bool isFetchingTop = false;
    bool isFetchingBottom = false;

    QPersistentModelIndex anchorIndex;
    int anchorDistanceFromBottom = 0;

    bool atBottom = false;
};
} // namespace UI
} // namespace Acheron
