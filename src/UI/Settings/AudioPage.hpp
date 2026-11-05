#pragma once

#include <QWidget>

class QComboBox;

namespace Acheron {
namespace UI {

[[nodiscard]] QComboBox *makeAudioBackendCombo(QWidget *parent);

class AudioPage : public QWidget
{
    Q_OBJECT
public:
    explicit AudioPage(QWidget *parent = nullptr);
};

} // namespace UI
} // namespace Acheron
