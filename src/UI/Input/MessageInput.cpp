#include "MessageInput.hpp"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QAbstractTextDocumentLayout>

namespace Acheron {
namespace UI {

ChatTextEdit::ChatTextEdit(QWidget *parent) : QTextEdit(parent)
{
    document()->setDocumentMargin(0);

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
    QTextEdit::keyPressEvent(e);
}

MessageInput::MessageInput(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);

    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);

    textEdit = new ChatTextEdit(this);
    setFocusProxy(textEdit);

    connect(textEdit, &ChatTextEdit::returnPressed, [this]() {
        QString txt = textEdit->toPlainText().trimmed();
        if (!txt.isEmpty()) {
            emit sendMessage(txt);
            clear();
        }
    });

    connect(textEdit->document(), &QTextDocument::contentsChanged, this,
            &MessageInput::adjustHeight);

    layout->addWidget(textEdit);

    adjustHeight();
}

void MessageInput::setPlaceholder(const QString &name)
{
    textEdit->setPlaceholderText("Message #" + name);
}

void MessageInput::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void MessageInput::clear()
{
    textEdit->clear();
    adjustHeight();
}

void MessageInput::adjustHeight()
{
    int contentHeight = textEdit->document()->size().height();

    int newHeight = contentHeight + contentHeight;

    if (newHeight < 44)
        newHeight = 44;
    if (newHeight > 200)
        newHeight = 200;

    if (newHeight >= 200) {
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    } else {
        textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    textEdit->setFixedHeight(newHeight);

    setFixedHeight(newHeight + 24);
}

} // namespace UI
} // namespace Acheron