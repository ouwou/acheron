#include "VolumeMeter.hpp"

#include <QLinearGradient>
#include <QPainter>

namespace Acheron {
namespace UI {

VolumeMeter::VolumeMeter(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void VolumeMeter::setLevel(float rms)
{
    if (rms == level)
        return;
    level = rms;
    update();
}

void VolumeMeter::setThreshold(std::optional<float> value)
{
    threshold = value;
    update();
}

void VolumeMeter::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const int filled = static_cast<int>(qBound(0.0f, level / MAX_RMS, 1.0f) * static_cast<float>(width()));
    if (filled > 0) {
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0.0, QColor(0x43, 0xb5, 0x81));
        gradient.setColorAt(0.5, QColor(0xfa, 0xa6, 0x1a));
        gradient.setColorAt(1.0, QColor(0xf0, 0x47, 0x47));
        painter.fillRect(0, 0, filled, height(), gradient);
    }

    if (threshold) {
        const int x = static_cast<int>(qBound(0.0f, *threshold / MAX_RMS, 1.0f) * static_cast<float>(width()));
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawLine(x, 0, x, height());
    }
}

} // namespace UI
} // namespace Acheron
