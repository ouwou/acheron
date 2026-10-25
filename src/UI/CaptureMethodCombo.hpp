#pragma once

#include <QComboBox>
#include <QCoreApplication>
#include <QStandardItemModel>

#include "Core/Video/CaptureSources.hpp"

namespace Acheron {
namespace UI {

// the methods this platform has, those that cant work greyed out, and the remembered one chosen
inline QComboBox *makeCaptureMethodCombo(QWidget *parent)
{
    using Core::Video::CaptureMethod;
    const auto text = [](const char *name) { return QCoreApplication::translate("Acheron::UI::CaptureMethodCombo", name); };

    auto *combo = new QComboBox(parent);
    combo->addItem(text("Automatic"), static_cast<int>(CaptureMethod::Auto));
#ifdef Q_OS_WIN
    combo->addItem(text("Graphics Capture"), static_cast<int>(CaptureMethod::GraphicsCapture));
    combo->addItem(text("Desktop Duplication"), static_cast<int>(CaptureMethod::DesktopDuplication));
    combo->addItem(text("Screen Copy (GDI)"), static_cast<int>(CaptureMethod::ScreenCopy));
#else
    combo->addItem(text("Desktop Portal"), static_cast<int>(CaptureMethod::Portal));
    combo->addItem(text("X11"), static_cast<int>(CaptureMethod::X11));
#endif
    if (auto *methods = qobject_cast<QStandardItemModel *>(combo->model())) {
        for (int i = 0; i < combo->count(); i++)
            methods->item(i)->setEnabled(Core::Video::captureMethodUsable(static_cast<CaptureMethod>(combo->itemData(i).toInt())));
    }
    const CaptureMethod remembered = Core::Video::captureMethod();
    combo->setCurrentIndex(combo->findData(static_cast<int>(Core::Video::captureMethodUsable(remembered) ? remembered : CaptureMethod::Auto)));
    return combo;
}

inline Core::Video::CaptureMethod chosenCaptureMethod(const QComboBox *combo)
{
    return static_cast<Core::Video::CaptureMethod>(combo->currentData().toInt());
}

} // namespace UI
} // namespace Acheron
