#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QMenu;
class QPushButton;
class QToolButton;

namespace Acheron {
namespace Core {
class CameraSender;
class StreamBroadcaster;
} // namespace Core

namespace UI {

class ElidedLabel;

void fillShareQualityMenu(QMenu *menu, Core::StreamBroadcaster *broadcaster);

class CallControlsBar : public QWidget
{
    Q_OBJECT
public:
    CallControlsBar(Core::StreamBroadcaster *broadcaster, Core::CameraSender *camera, bool nonVideoShown, QWidget *parent);

    struct Chrome
    {
        bool poppedOut = false;
        bool fullScreen = false;
        bool collapsed = false;
        bool videoPresent = false;
    };
    void apply(const Chrome &chrome);
    void setSummary(const QString &text);

    [[nodiscard]] bool nonVideoShown() const;
    [[nodiscard]] bool statsShown() const;

signals:
    void collapseClicked();
    void nonVideoShownChanged(bool shown);
    void statsShownChanged(bool shown);
    void popOutClicked();
    void fullScreenClicked();

private:
    void onShareClicked();
    void onCameraClicked();
    void fillCameraMenu();
    void chooseCamera(const QString &deviceId);

    QPointer<Core::StreamBroadcaster> broadcaster;
    QPointer<Core::CameraSender> camera;
    bool captureAvailable = false;
    bool cameraAvailable = false;

    QToolButton *collapseButton;
    ElidedLabel *summaryLabel;
    QToolButton *shareButton;
    QMenu *shareQualityMenu;
    QToolButton *cameraButton;
    QMenu *cameraMenu;
    QCheckBox *nonVideoCheck;
    QPushButton *statsButton;
    QPushButton *popOutButton;
    QPushButton *fullScreenButton;
};

} // namespace UI
} // namespace Acheron
