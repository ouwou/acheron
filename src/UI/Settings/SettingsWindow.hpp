#pragma once

#include <QtWidgets>

namespace Acheron {
namespace UI {

class SettingsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

signals:
    void channelListModeChanged(bool classic);
    void animateEmojiChanged(bool enabled);
    void animateStickersChanged(bool enabled);
    void animationCacheLimitChanged(int megabytes);
    void animationSizeLimitChanged(int megabytes);

private:
    void setupUi();

    QListWidget *categoryList;
    QStackedWidget *pages;
};

} // namespace UI
} // namespace Acheron
