#pragma once

#include <QtWidgets>

namespace Acheron {
namespace UI {

class SettingsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

private:
    void setupUi();
    void loadSettings();

    QListWidget *categoryList;
    QStackedWidget *pages;

    // general
    QCheckBox *inMemoryCacheCheckbox;
};

} // namespace UI
} // namespace Acheron
