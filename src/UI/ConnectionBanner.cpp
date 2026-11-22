#include "ConnectionBanner.hpp"
#include <QHBoxLayout>
#include <QPainter>

namespace Acheron {
namespace UI {

ConnectionBanner::ConnectionBanner(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    label = new QLabel(this);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    dotTimer = new QTimer(this);
    dotTimer->setInterval(400);
    connect(dotTimer, &QTimer::timeout, this, &ConnectionBanner::updateDots);

    setVisible(false);
    setFixedHeight(28);
    applyPaletteColors();
}

void ConnectionBanner::applyPaletteColors()
{
    QPalette pal = palette();
    QColor bg = pal.color(QPalette::Highlight);
    QColor fg = pal.color(QPalette::HighlightedText);

    label->setStyleSheet(
            QString("color: %1; font-size: 12px; font-weight: 500; padding: 6px 0px;")
                    .arg(fg.name()));

    setAutoFillBackground(false);
}

void ConnectionBanner::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), palette().highlight());
}

void ConnectionBanner::showReconnecting(int attempt)
{
    if (attempt > 0)
        baseText = QString("Reconnecting (attempt %1)").arg(attempt);
    else
        baseText = "Reconnecting";

    dotCount = 0;
    label->setText(baseText + "...");
    setVisible(true);
    dotTimer->start();
}

void ConnectionBanner::hide()
{
    dotTimer->stop();
    setVisible(false);
}

void ConnectionBanner::updateDots()
{
    dotCount = (dotCount + 1) % 4;
    QString dots = QString(".").repeated(dotCount);
    QString padding = QString(" ").repeated(3 - dotCount);
    label->setText(baseText + dots + padding);
}

} // namespace UI
} // namespace Acheron
