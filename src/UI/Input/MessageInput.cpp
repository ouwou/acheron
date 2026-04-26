#include "MessageInput.hpp"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QAbstractTextDocumentLayout>
#include <QToolButton>
#include <QMimeData>
#include <QBuffer>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QMimeDatabase>

namespace Acheron {
namespace UI {

ChatTextEdit::ChatTextEdit(QWidget *parent) : QTextEdit(parent)
{
    document()->setDocumentMargin(0);
    setAcceptRichText(false);

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setPlaceholderText("Message...");
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    QTextEdit::keyPressEvent(e);
}

void ChatTextEdit::insertFromMimeData(const QMimeData *source)
{
    if (source && source->hasImage()) {
        QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) {
            emit imagePasted(img);
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
    replyCancelButton->setText(QStringLiteral("\u00D7")); // multiplication sign as close icon
    replyCancelButton->setFixedSize(16, 16);
    replyCancelButton->setStyleSheet(
            "QToolButton { border: none; color: #b5bac1; font-size: 14px; }"
            "QToolButton:hover { color: #ffffff; }");
    replyLayout->addWidget(replyCancelButton);

    connect(replyCancelButton, &QToolButton::clicked, this, &MessageInput::clearReplyTarget);

    outerLayout->addWidget(replyBar);

    // Attachments bar
    attachmentsBar = new QWidget(this);
    attachmentsBar->setVisible(false);
    auto *attLayout = new QHBoxLayout(attachmentsBar);
    attLayout->setContentsMargins(8, 4, 4, 2);
    attLayout->setSpacing(4);

    attachmentsLabel = new QLabel(attachmentsBar);
    attachmentsLabel->setStyleSheet("color: #b5bac1; font-size: 12px;");
    attLayout->addWidget(attachmentsLabel, 1);

    attachmentsCancelButton = new QToolButton(attachmentsBar);
    attachmentsCancelButton->setText(QStringLiteral("\u00D7"));
    attachmentsCancelButton->setFixedSize(16, 16);
    attachmentsCancelButton->setStyleSheet(
            "QToolButton { border: none; color: #b5bac1; font-size: 14px; }"
            "QToolButton:hover { color: #ffffff; }");
    attLayout->addWidget(attachmentsCancelButton);

    connect(attachmentsCancelButton, &QToolButton::clicked, [this]() {
        pendingUploads.clear();
        updateAttachmentsUi();
        adjustHeight();
        textEdit->setFocus();
    });

    outerLayout->addWidget(attachmentsBar);

    // Text edit
    auto *inputContainer = new QWidget(this);
    auto *inputLayout = new QHBoxLayout(inputContainer);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(0);

    attachButton = new QToolButton(inputContainer);
    attachButton->setText("+");
    attachButton->setFixedWidth(28);
    attachButton->setStyleSheet(
            "QToolButton { border: none; color: #b5bac1; font-size: 16px; }"
            "QToolButton:hover { color: #ffffff; }");

    textEdit = new ChatTextEdit(inputContainer);
    setFocusProxy(textEdit);

    connect(attachButton, &QToolButton::clicked, this, [this]() {
        QStringList paths =
                QFileDialog::getOpenFileNames(this, tr("Attach files"), QString(), tr("All Files (*.*)"));
        if (paths.isEmpty())
            return;

        QMimeDatabase mimeDb;
        for (const QString &path : paths) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;

            Discord::FileUpload upload;
            upload.filename = QFileInfo(path).fileName();
            upload.data = f.readAll();
            upload.mimeType = mimeDb.mimeTypeForFile(path).name();
            if (upload.mimeType.isEmpty())
                upload.mimeType = "application/octet-stream";

            pendingUploads.append(upload);
        }

        updateAttachmentsUi();
        adjustHeight();
        textEdit->setFocus();
    });

    connect(textEdit, &ChatTextEdit::returnPressed, [this]() {
        if (sendBlocked)
            return;
        QString txt = textEdit->toPlainText().trimmed();
        if (!txt.isEmpty() || !pendingUploads.isEmpty()) {
            emit sendMessage(txt, pendingUploads);
            clear();
        }
    });

    connect(textEdit, &ChatTextEdit::escapePressed, this, &MessageInput::clearReplyTarget);

    connect(textEdit, &ChatTextEdit::imagePasted, this, [this](const QImage &image) {
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            return;

        Discord::FileUpload upload;
        upload.mimeType = "image/png";
        upload.data = data;
        upload.filename = QString("pasted-%1.png")
                                  .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss"));

        pendingUploads.append(upload);
        updateAttachmentsUi();
        adjustHeight();
    });

    connect(textEdit->document(), &QTextDocument::contentsChanged, this,
            &MessageInput::adjustHeight);

    inputLayout->addWidget(attachButton);
    inputLayout->addWidget(textEdit);
    outerLayout->addWidget(inputContainer);

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
    pendingUploads.clear();
    updateAttachmentsUi();
    adjustHeight();
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

void MessageInput::adjustHeight()
{
    int contentHeight = textEdit->document()->size().height();

    int newHeight = contentHeight + contentHeight;

    if (newHeight < 44)
        newHeight = 44;
    if (newHeight > 200)
        newHeight = 200;

    if (newHeight >= 200)
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    else
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    textEdit->setFixedHeight(newHeight);

    int totalHeight = newHeight + 12;
    if (replyBar->isVisible())
        totalHeight += replyBar->sizeHint().height();
    if (attachmentsBar->isVisible())
        totalHeight += attachmentsBar->sizeHint().height();

    setFixedHeight(totalHeight);
}

void MessageInput::updateAttachmentsUi()
{
    if (pendingUploads.isEmpty()) {
        attachmentsBar->setVisible(false);
        return;
    }

    int count = pendingUploads.size();
    attachmentsLabel->setText(tr("%1 file%2 attached")
                                      .arg(count)
                                      .arg(count == 1 ? QString() : QStringLiteral("s")));
    attachmentsBar->setVisible(true);
}

} // namespace UI
} // namespace Acheron