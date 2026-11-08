#pragma once

#include <QCoreApplication>
#include <QToolButton>

namespace Acheron {
namespace UI {

inline QToolButton *makeAdvancedToggle(QWidget *section, QWidget *parent)
{
    auto *toggle = new QToolButton(parent);
    toggle->setText(QCoreApplication::translate("Acheron::UI::AdvancedToggle", "Advanced"));
    toggle->setCheckable(true);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setAutoRaise(true);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setStyleSheet(
            "QToolButton { font-size: 10px; font-weight: bold; text-transform: uppercase;"
            "  color: palette(text); background: transparent; border: none; padding: 4px 0; }"
            "QToolButton:hover { color: palette(highlight); }");

    section->setVisible(false);
    QObject::connect(toggle, &QToolButton::toggled, section, [toggle, section](bool open) {
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        section->setVisible(open);
    });
    return toggle;
}

} // namespace UI
} // namespace Acheron
