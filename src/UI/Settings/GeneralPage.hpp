#pragma once

#include <QWidget>

class QCheckBox;

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
};

} // namespace UI
} // namespace Acheron
