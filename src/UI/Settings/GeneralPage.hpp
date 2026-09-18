#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;

namespace Acheron {
namespace UI {

class GeneralPage : public QWidget
{
    Q_OBJECT
public:
    explicit GeneralPage(QWidget *parent = nullptr);

signals:
    void animateEmojiChanged(bool enabled);

private:
    QCheckBox *inMemoryCacheCheckbox;
    QCheckBox *animateEmojiCheckbox;
    QComboBox *fontEngineCombo = nullptr;
};

} // namespace UI
} // namespace Acheron
