#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>

#include "Discord/AV/VoiceClient.hpp"

namespace Acheron {
namespace Core {
namespace AV {
class VoiceManager;
}
} // namespace Core
namespace UI {

class VoiceWindow;

class VoiceStatusBar : public QWidget
{
    Q_OBJECT
public:
    explicit VoiceStatusBar(QWidget *parent = nullptr);

    void setVoiceManager(Core::AV::VoiceManager *manager);
    void setChannelName(const QString &name);
    void updateConnectionState();

signals:
    void disconnectRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void setupUi();
    void disconnectManager();
    void toggleVoiceWindow();
    void showVoiceWindow();

    Core::AV::VoiceManager *voiceManager = nullptr;
    QList<QMetaObject::Connection> managerConnections;

    QLabel *statusDot;
    QLabel *statusLabel;
    QLabel *channelLabel;
    QPushButton *disconnectBtn;

    VoiceWindow *voiceWindow = nullptr;
    bool wasDisconnected = true;
};

} // namespace UI
} // namespace Acheron
