#include "MessageInput.hpp"
#include "AttachmentPreviewPanel.hpp"
#include "TextEdgeNavigation.hpp"

#include "Core/Emoji/UnicodeEmojiIndex.hpp"
#include "Core/Theme/Icons.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QAbstractTextDocumentLayout>
#include <QDragEnterEvent>
#include <QImage>
#include <QMimeData>
#include <QToolButton>

#include <algorithm>

namespace Acheron {
namespace UI {

namespace {

constexpr int MinTextEditHeight = 44;
constexpr int MaxTextEditHeight = 200;
constexpr int EmojiButtonSize = 28;
constexpr int EmojiButtonIconSize = 20;
constexpr int EmojiButtonGap = 4;

} // namespace

ChatTextEdit::ChatTextEdit(QWidget *parent) : QTextEdit(parent)
{
    setObjectName("MessageInput");
    document()->setDocumentMargin(0);
    setAcceptRichText(false);

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setPlaceholderText("Message...");
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    emojiPickerButton = new QToolButton(this);
    emojiPickerButton->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::Smile, Core::Theme::Token::PlaceholderText));
    emojiPickerButton->setIconSize(QSize(EmojiButtonIconSize, EmojiButtonIconSize));
    emojiPickerButton->setFixedSize(EmojiButtonSize, EmojiButtonSize);
    emojiPickerButton->setToolTip(tr("Select emoji"));
    emojiPickerButton->setAutoRaise(true);
    emojiPickerButton->setFocusPolicy(Qt::NoFocus);
    emojiPickerButton->setCursor(Qt::PointingHandCursor);
    setViewportMargins(0, 0, EmojiButtonSize + EmojiButtonGap, 0);
}

void ChatTextEdit::resizeEvent(QResizeEvent *e)
{
    QTextEdit::resizeEvent(e);
    emojiPickerButton->move(viewport()->geometry().right() + 1 + EmojiButtonGap, (MinTextEditHeight - EmojiButtonSize) / 2);
}

void ChatTextEdit::changeEvent(QEvent *e)
{
    QTextEdit::changeEvent(e);
    if (e->type() != QEvent::EnabledChange)
        return;

    emojiPickerButton->setVisible(isEnabled());
    setViewportMargins(0, 0, isEnabled() ? EmojiButtonSize + EmojiButtonGap : 0, 0);
}

void ChatTextEdit::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (!(e->modifiers() & Qt::ShiftModifier)) {
            emit returnPressed();
            return;
        }
    }
    if (e->key() == Qt::Key_Escape) {
        emit escapePressed();
        return;
    }
    if (e->key() == Qt::Key_Up && e->modifiers() == Qt::NoModifier && document()->isEmpty()) {
        emit editLastMessageRequested();
        return;
    }
    if (moveCursorToTextEdgeFromOuterLine(this, e))
        return;
    QTextEdit::keyPressEvent(e);
}

static bool mimeHasLocalFiles(const QMimeData *source)
{
    if (!source->hasUrls())
        return false;
    const auto urls = source->urls();
    return std::any_of(urls.begin(), urls.end(),
                       [](const QUrl &url) { return url.isLocalFile(); });
}

bool ChatTextEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return mimeHasLocalFiles(source) || source->hasImage() || QTextEdit::canInsertFromMimeData(source);
}

void ChatTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (mimeHasLocalFiles(source)) {
        emit filesPasted(source->urls());
        return;
    }
    if (source->hasImage()) {
        QImage image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull()) {
            emit imagePasted(image);
            return;
        }
    }
    QTextEdit::insertFromMimeData(source);
}

MessageInput::MessageInput(QWidget *parent) : QWidget(parent)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(4, 0, 4, 0);
    outerLayout->setSpacing(0);

    // Reply bar
    replyBar = new QWidget(this);
    replyBar->setVisible(false);
    auto *replyLayout = new QHBoxLayout(replyBar);
    replyLayout->setContentsMargins(8, 4, 4, 2);
    replyLayout->setSpacing(4);

    replyLabel = new QLabel(replyBar);
    replyLabel->setStyleSheet("color: #b5bac1; font-size: 12px;");
    replyLayout->addWidget(replyLabel, 1);

    replyCancelButton = new QToolButton(replyBar);
    replyCancelButton->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::X, Core::Theme::Token::PlaceholderText));
    replyCancelButton->setIconSize(QSize(14, 14));
    replyCancelButton->setFixedSize(16, 16);
    replyCancelButton->setAutoRaise(true);
    replyCancelButton->setCursor(Qt::PointingHandCursor);
    replyCancelButton->setStyleSheet("QToolButton { border: none; }");
    replyLayout->addWidget(replyCancelButton);

    connect(replyCancelButton, &QToolButton::clicked, this, &MessageInput::clearReplyTarget);

    outerLayout->addWidget(replyBar);

    attachmentPanel = new AttachmentPreviewPanel(this);
    outerLayout->addWidget(attachmentPanel);

    // Text edit
    auto *inputContainer = new QWidget(this);
    auto *inputLayout = new QHBoxLayout(inputContainer);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(0);

    textEdit = new ChatTextEdit(inputContainer);
    setFocusProxy(textEdit);

    connect(textEdit, &ChatTextEdit::returnPressed, [this]() {
        if (sendBlocked)
            return;
        QString txt = textEdit->toPlainText().trimmed();
        if (txt.isEmpty() && !attachmentPanel->hasAttachments())
            return;
        txt = Core::UnicodeEmojiIndex::instance().translateNamesToSurrogates(txt);
        emit sendMessage(txt, attachmentPanel->attachments());
        clear();
    });

    connect(textEdit, &ChatTextEdit::editLastMessageRequested, this, &MessageInput::editLastMessageRequested);
    connect(textEdit->emojiButton(), &QToolButton::clicked, this, &MessageInput::requestEmojiPicker);

    connect(textEdit, &ChatTextEdit::escapePressed, this, [this]() {
        clearReplyTarget();
        attachmentPanel->clearAttachments();
    });

    connect(textEdit->document(), &QTextDocument::contentsChanged, this,
            &MessageInput::adjustHeight);

    autocomplete = new EmojiAutocomplete(textEdit, this);

    connect(textEdit, &ChatTextEdit::filesPasted, this, &MessageInput::queueAttachments);
    connect(textEdit, &ChatTextEdit::imagePasted, attachmentPanel, &AttachmentPreviewPanel::addImage);
    connect(attachmentPanel, &AttachmentPreviewPanel::attachmentsChanged, this, &MessageInput::adjustHeight);

    inputLayout->addWidget(textEdit);
    outerLayout->addWidget(inputContainer);

    setAcceptDrops(true);

    adjustHeight();
}

void MessageInput::setPlaceholder(const QString &name)
{
    textEdit->setPlaceholderText(name);
}

void MessageInput::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void MessageInput::clear()
{
    textEdit->clear();
    clearReplyTarget();
    attachmentPanel->clearAttachments();
    adjustHeight();
}

void MessageInput::queueAttachments(const QList<QUrl> &urls)
{
    attachmentPanel->addFiles(urls);
    textEdit->setFocus();
}

void MessageInput::setMaxUploadSize(qint64 bytes)
{
    attachmentPanel->setMaxFileSize(bytes);
}

void MessageInput::dragEnterEvent(QDragEnterEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (event->mimeData()->hasUrls() &&
        std::any_of(urls.begin(), urls.end(),
                    [](const QUrl &url) { return url.isLocalFile(); }))
        event->acceptProposedAction();
}

void MessageInput::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    event->acceptProposedAction();
    queueAttachments(event->mimeData()->urls());
}

void MessageInput::setReplyTarget(Core::Snowflake messageId, const QString &authorName,
                                  const QString &contentSnippet)
{
    replyMessageId = messageId;
    QString snippet = contentSnippet;
    snippet.replace('\n', ' ');
    if (snippet.length() > 100)
        snippet = snippet.left(100) + "...";

    replyLabel->setText(tr("Replying to <b>%1</b> %2").arg(authorName, snippet));
    replyBar->setVisible(true);
    adjustHeight();
    textEdit->setFocus();
}

void MessageInput::clearReplyTarget()
{
    if (!replyMessageId.isValid())
        return;

    replyMessageId = Core::Snowflake::Invalid;
    replyBar->setVisible(false);
    adjustHeight();
}

void MessageInput::setSendBlocked(bool blocked)
{
    sendBlocked = blocked;
}

void MessageInput::insertText(const QString &text)
{
    textEdit->insertPlainText(text);
    textEdit->setFocus();
}

void MessageInput::requestEmojiPicker()
{
    const QToolButton *button = textEdit->emojiButton();
    if (button->isVisible())
        emit emojiPickerRequested(QRect(button->mapToGlobal(QPoint(0, 0)), button->size()));
}

void MessageInput::setEmojiPickerOpen(bool open)
{
    textEdit->emojiButton()->setDown(open);
}

void MessageInput::adjustHeight()
{
    int contentHeight = textEdit->document()->size().height();

    int newHeight = contentHeight + contentHeight;

    if (newHeight < MinTextEditHeight)
        newHeight = MinTextEditHeight;
    if (newHeight > MaxTextEditHeight)
        newHeight = MaxTextEditHeight;

    if (newHeight >= MaxTextEditHeight)
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    else
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    textEdit->setFixedHeight(newHeight);

    int totalHeight = newHeight + 12;
    if (replyBar->isVisible())
        totalHeight += replyBar->sizeHint().height();
    if (attachmentPanel->isVisible())
        totalHeight += attachmentPanel->sizeHint().height();

    setFixedHeight(totalHeight);
}

} // namespace UI
} // namespace Acheron