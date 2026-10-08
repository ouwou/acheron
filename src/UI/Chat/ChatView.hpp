#pragma once

#include <QtWidgets>
#include <QClipboard>
#include <QGuiApplication>

#include <optional>

#include "ChatLayout.hpp"
#include "ChatModel.hpp"
#include "MessageActionBar.hpp"
#include "Core/Emoji/EmojiManager.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace UI {

class FrameAnimator;
class GifPlayback;
class ImageViewer;
class InlineVideoController;
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

class JumpToPresentBar : public QWidget
{
    Q_OBJECT
public:
    explicit JumpToPresentBar(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void applyTheme();

    QLabel *label = nullptr;
    QPushButton *button = nullptr;
};

class ChatView : public QListView
{
    Q_OBJECT
public:
    ChatView(QWidget *parent = nullptr);

    void setImageManager(Core::ImageManager *manager);
    void setEmojiManager(Core::EmojiManager *manager, Core::Snowflake accountId);

    int hoveredRowAtPaint() const { return hoveredRow; }
    int hoveredCharIndexAtPaint() const { return hoveredChar; }
    bool replyBarHoveredAtPaint() const { return hoveredReplyBar; }
    int hoveredMessageRowAtPaint() const { return hoveredMessage.isValid() ? hoveredMessage.row() : -1; }
    int editingRow() const { return currentEditingIndex.isValid() ? currentEditingIndex.row() : -1; }

    // jump flash
    int highlightedRow() const;
    qreal highlightOpacity() const { return highlightAlpha; }

    [[nodiscard]] InlineVideoController *videoController() const { return video; }
    [[nodiscard]] FrameAnimator *frameAnimator() const { return animator; }
    [[nodiscard]] GifPlayback *gifPlayback() const { return gifs; }

    static constexpr int InlineEditMinHeight = 60;

    bool hasTextSelection() const;

    ChatCursor selectionStart() const;
    ChatCursor selectionEnd() const;

    void setModel(QAbstractItemModel *model) override;

    void setCurrentUserId(Core::Snowflake userId);
    void setCanPinMessages(bool canPin);
    void setCanManageMessages(bool canManage);
    void setCanAddReactions(bool canReact);
    void setReactionPickerOpen(bool open);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void clearSelection();
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void updateGeometries() override;

signals:
    void historyRequested();
    void futureRequested();
    void jumpRequested(Core::Snowflake messageId);
    void presentRequested();
    void atBottomChanged(bool atBottom);
    void editMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId, const QString &currentContent);
    void deleteMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void pinMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void replyToMessageRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void reactionPickerRequested(Core::Snowflake channelId, Core::Snowflake messageId, const QRect &globalAnchor);
    void cancelUploadRequested(Core::Snowflake channelId, Core::Snowflake messageId);
    void filesDropped(const QList<QUrl> &urls);
    void reactionToggleRequested(Core::Snowflake channelId, Core::Snowflake messageId, const Acheron::Discord::Emoji &emoji, bool currentlyReacted, bool isBurst, Acheron::Discord::Client::ReactionLocation location);
    void channelMentionClicked(Core::Snowflake channelId);
    void messageLinkClicked(Core::Snowflake channelId, Core::Snowflake messageId);
    void userContextMenuRequested(Core::Snowflake userId, QPoint globalPos);
    void inlineEditFinished();

public slots:
    void onHistoryRequestFinished();
    void onFutureRequestFinished(bool loadedMore);
    void editLastOwnMessage();
    void jumpToMessage(Core::Snowflake messageId);
    void openLink(const QString &url);
    void jumpToPresent();

private slots:
    void onScrollBarValueChanged(int value);
    void onModelReset();
    void onAtLatestChanged(bool atLatest);
    void onRowsAboutToBeInserted(const QModelIndex &parent, int start, int end);
    void onRowsInserted(const QModelIndex &parent, int start, int end);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    void onActionBarTriggered(MessageActionBar::Action action);
    void onActionBarMoreRequested(const QPoint &buttonTopLeft);
    void onActionBarQuickReaction(const Core::PickerEmoji &emoji, bool reacted);
    void onActionBarReactionPickerRequested();

private:
    void copySelectedText();
    void copyMessageContent(const QModelIndex &index);
    static void copyMessageId(Core::Snowflake messageId);
    void copyMessageLink(Core::Snowflake messageId);
    void copyImage(const QUrl &proxyUrl, const QPixmap &preview);
    void saveMedia(const QUrl &url, const QString &filename);
    void startInlineEdit(const QModelIndex &index);
    void commitInlineEdit();
    void cancelInlineEdit();
    bool scrollToMessage(Core::Snowflake messageId);
    void flashMessage(Core::Snowflake messageId);
    bool modelAtLatest() const;
    void updateScrollState();
    void maybeRequestFuture();
    void positionJumpToPresentBar();
    void updateJumpToPresentBar();
    bool canShowActionBar(const QModelIndex &index) const;
    bool canDeleteMessage(const QModelIndex &index) const;
    QModelIndex messageUnderCursor(const QPoint &globalPos) const;
    QPoint actionBarPosition(const QModelIndex &index) const;
    void updateHoveredMessage();
    void addMessageAction(QMenu &menu, MessageActionBar::Action action, Core::Snowflake messageId);
    void triggerMessageAction(MessageActionBar::Action action, Core::Snowflake messageId);
    void execMessageMenu(QMenu &menu, const QPoint &globalPos);
    [[nodiscard]] bool canReactTo(const QModelIndex &index) const;
    [[nodiscard]] QList<MessageActionBar::QuickReaction> quickReactionsFor(const QModelIndex &index) const;
    [[nodiscard]] QIcon reactionMenuIcon(const Core::PickerEmoji &emoji) const;
    void addReactionMenu(QMenu &menu, Core::Snowflake messageId);
    void requestReactionPicker(Core::Snowflake messageId);
    void releaseHoverHold(const QPoint &globalPos);

    InlineVideoController *video = nullptr;
    FrameAnimator *animator = nullptr;
    GifPlayback *gifs = nullptr;
    Core::ImageManager *imageManager = nullptr;
    QPointer<Core::EmojiManager> emojis;

    QTextEdit *inlineEditWidget = nullptr;
    Core::Snowflake currentEditingMessageId = Core::Snowflake::Invalid;
    QModelIndex currentEditingIndex;

    JumpToPresentBar *jumpToPresentBar = nullptr;
    MessageActionBar *actionBar = nullptr;
    QPersistentModelIndex hoveredMessage;
    std::optional<QPoint> hoverHeldAt;
    Core::Snowflake actionBarMessageId = Core::Snowflake::Invalid;
    Core::Snowflake pendingJumpMessageId = Core::Snowflake::Invalid;
    Core::Snowflake highlightedMessageId = Core::Snowflake::Invalid;
    qreal highlightAlpha = 0.0;
    QSequentialAnimationGroup *highlightAnimation = nullptr;

    int hoveredRow;
    int hoveredChar;
    bool hoveredReplyBar = false;

    ChatCursor selectionAnchor;
    ChatCursor selectionHead;

    bool isFetchingTop = false;
    bool isFetchingBottom = false;

    QPersistentModelIndex anchorIndex;
    int anchorDistanceFromBottom = 0;

    void setAtBottom(bool value);
    bool atBottom = true;

    Core::Snowflake currentUserId = Core::Snowflake::Invalid;
    bool canPinMessages = false;
    bool canManageMessages = false;
    bool canAddReactions = false;
};
} // namespace UI
} // namespace Acheron
