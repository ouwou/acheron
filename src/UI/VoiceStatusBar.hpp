#pragma once

#include <QWidget>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <functional>

#include "Core/Snowflake.hpp"
#include "Discord/AV/VoiceClient.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
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
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    explicit VoiceStatusBar(QWidget *parent = nullptr);

    void setVoiceManager(Core::AV::VoiceManager *manager);
    void setNameResolver(NameResolver resolver);
    void setAvatarResolver(AvatarResolver resolver);
    void setImageManager(Core::ImageManager *manager);
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
    void configureVoiceWindow();

    Core::AV::VoiceManager *voiceManager = nullptr;
    Core::ImageManager *imageManager = nullptr;
    NameResolver nameResolver;
    AvatarResolver avatarResolver;

    QLabel *statusDot;
    QLabel *statusLabel;
    QLabel *channelLabel;
    QPushButton *disconnectBtn;

    QPointer<VoiceWindow> voiceWindow;
    QTimer closeTimer;
    bool wasDisconnected = true;
};

} // namespace UI
} // namespace Acheron
