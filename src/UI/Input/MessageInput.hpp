#pragma once

#include <QWidget>
#include <QTextEdit>

namespace Acheron {
namespace UI {

class ChatTextEdit : public QTextEdit
{
    Q_OBJECT
public:
    explicit ChatTextEdit(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
signals:
    void returnPressed();
};

class MessageInput : public QWidget
{
    Q_OBJECT
public:
    explicit MessageInput(QWidget *parent = nullptr);
    void clear();
    void setPlaceholder(const QString &name);

protected:
    void resizeEvent(QResizeEvent *event) override;

signals:
    void sendMessage(const QString &text);

private:
    ChatTextEdit *textEdit;
    void adjustHeight();
};

} // namespace UI
} // namespace Acheron