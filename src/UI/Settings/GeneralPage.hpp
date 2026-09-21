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
    void animateStickersChanged(bool enabled);
    void animationCacheLimitChanged(int megabytes);
    void animationSizeLimitChanged(int megabytes);

private:
    QCheckBox *inMemoryCacheCheckbox;
    QCheckBox *animateEmojiCheckbox;
    QCheckBox *animateStickersCheckbox;
    QComboBox *fontEngineCombo = nullptr;
};

} // namespace UI
} // namespace Acheron
