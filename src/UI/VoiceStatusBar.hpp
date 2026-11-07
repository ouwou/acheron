#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QMenu;
class QToolButton;

namespace Acheron {
namespace Core {
namespace Audio {
class VoiceManager;
}
} // namespace Core

namespace UI {

class VoiceStatusBar : public QWidget
{
    Q_OBJECT
public:
    explicit VoiceStatusBar(QWidget *parent = nullptr);

    void setVoiceManager(Core::Audio::VoiceManager *manager);
    void setChannelName(const QString &name);

signals:
    void disconnectRequested();
    void callRevealRequested();
    void voiceSettingsRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void updateConnectionState();
    void updateSelfAudio();
    void updatePrivacy();

    QPointer<Core::Audio::VoiceManager> voiceManager;

    QLabel *statusDot;
    QLabel *statusLabel;
    QLabel *channelLabel;
    QToolButton *privacyButton;
    QToolButton *micButton;
    QMenu *deviceMenu;
    QToolButton *deafenButton;
    QToolButton *disconnectButton;
};

} // namespace UI
} // namespace Acheron
