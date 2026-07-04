#include <QVBoxLayout>
#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QMouseEvent>
#include <QEvent>

#include "BasePopup.hpp"

namespace Acheron {
namespace UI {

BasePopup::BasePopup(QWidget *parent) : QDialog(parent, Qt::FramelessWindowHint | Qt::Dialog)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    auto *overlayLayout = new QVBoxLayout(this);
    overlayLayout->setAlignment(Qt::AlignCenter);
    overlayLayout->setContentsMargins(20, 20, 20, 20);

    container = new QFrame(this);
    container->setObjectName("ContentFrame");
    container->setAutoFillBackground(true);
    container->setFrameShape(QFrame::StyledPanel);
    container->setFrameShadow(QFrame::Raised);

    container->setMinimumWidth(300);
    container->setMaximumWidth(600);
    container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(25);
    shadow->setXOffset(0);
    shadow->setYOffset(8);
    shadow->setColor(QColor(0, 0, 0, 100));
    container->setGraphicsEffect(shadow);

    overlayLayout->addWidget(container);
}

void BasePopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 110));
}

void BasePopup::mousePressEvent(QMouseEvent *event)
{
    if (!container->geometry().contains(event->pos())) {
        reject();
    } else {
        QDialog::mousePressEvent(event);
    }
}

void BasePopup::showEvent(QShowEvent *event)
{
    if (parentWidget() && parentWidget()->window()) {
        QWidget *topLevel = parentWidget()->window();
        setGeometry(topLevel->geometry());
        topLevel->installEventFilter(this);
    }
    QDialog::showEvent(event);
}

bool BasePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (parentWidget() && obj == parentWidget()->window()) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move) {
            setGeometry(parentWidget()->window()->geometry());
        }
    }
    return QDialog::eventFilter(obj, event);
}

} // namespace UI
} // namespace Acheron
