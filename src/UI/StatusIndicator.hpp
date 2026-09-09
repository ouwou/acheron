#pragma once

#include <QColor>
#include <QRect>
#include <QSize>
#include <QWidget>

#include "Core/Presence/PresenceBadge.hpp"

class QPainter;

namespace Acheron {
namespace UI {

namespace StatusIndicator {

QString tooltip(const Core::PresenceBadge &badge);

void paintOnAvatar(QPainter &painter, const QRect &avatarRect, int dotSize,
                   const Core::PresenceBadge &badge, const QColor &ringColor);

} // namespace StatusIndicator

class StatusDot : public QWidget
{
    Q_OBJECT
public:
    explicit StatusDot(QWidget *parent = nullptr) : QWidget(parent) {}

    void setBadge(const Core::PresenceBadge &badge);
    void setDotSize(int size);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Core::PresenceBadge badge;
    int dotSize = 12;
};

StatusDot *attachStatusDot(QWidget *parent, const QRect &avatarRect, int dotSize);

} // namespace UI
} // namespace Acheron
