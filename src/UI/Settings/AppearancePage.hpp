#pragma once

#include <QColor>
#include <QHash>
#include <QPushButton>
#include <QWidget>

#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Tokens.hpp"

class QCheckBox;
class QFontComboBox;
class QGroupBox;
class QSpinBox;
class QToolButton;

namespace Acheron {
namespace UI {

class AppearancePage : public QWidget
{
    Q_OBJECT
public:
    explicit AppearancePage(QWidget *parent = nullptr);

signals:
    void channelListModeChanged(bool classic);
    void channelListIndentChanged();
    void showAllTimestampsChanged(bool show);

private:
    void rebuildSwatches();
    void updateColorEditors();
    void refreshFontControls();
    void generateInto(const QColor &seed, int schemeIndex, bool dark);

    QHash<Core::Theme::Token, QPushButton *> swatches;
    QHash<Core::Theme::Token, QToolButton *> colorResets;
    QGroupBox *generatorGroup = nullptr;
    QCheckBox *systemStyle = nullptr;
    QHash<Core::Theme::FontRole, QFontComboBox *> familyCombos;
    QHash<Core::Theme::FontRole, QSpinBox *> sizeSpins;

    QColor seedColor;
    QPushButton *seedSwatch = nullptr;
};

} // namespace UI
} // namespace Acheron
