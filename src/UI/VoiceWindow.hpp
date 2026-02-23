#pragma once

#include <QWidget>
#include <QLabel>
#include <QSlider>
#include <QComboBox>
#include <QHash>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ImageManager;
namespace AV {
class VoiceManager;
}
} // namespace Core

namespace UI {

class VolumeMeter : public QWidget
{
    Q_OBJECT
public:
    explicit VolumeMeter(QWidget *parent = nullptr);

    void setLevel(float rms);
    void setThreshold(float threshold);

    QSize sizeHint() const override { return QSize(100, 12); }
    QSize minimumSizeHint() const override { return QSize(40, 12); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    float level = 0.0f;
    float threshold = -1.0f;

    static constexpr float MAX_RMS = 2000.0f;
};

class VoiceAvatar : public QWidget
{
    Q_OBJECT
public:
    explicit VoiceAvatar(QWidget *parent = nullptr);

    void setPixmap(const QPixmap &pm);
    void setSpeaking(bool speaking);

    QSize sizeHint() const override { return QSize(OUTER_SIZE, OUTER_SIZE); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap avatar;
    bool speaking = false;

    static constexpr int OUTER_SIZE = 32;
    static constexpr int BORDER_WIDTH = 2;
    static constexpr int AVATAR_RADIUS = 4;
};

class VoiceUserWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VoiceUserWidget(Core::Snowflake userId, QWidget *parent = nullptr);

    void setSpeaking(bool speaking);
    void setAudioLevel(float rms);
    void setVoiceStateFlags(bool selfMute, bool selfDeaf, bool serverMute, bool serverDeaf, bool suppress);
    void setLocallyMuted(bool muted);
    void setVolume(int pct);
    void setDisplayName(const QString &name);
    void setAvatar(const QPixmap &pm);
    void setDaveActive(bool active);
    int volume() const { return volumeSlider->value(); }

signals:
    void volumeChanged(Core::Snowflake userId, int pct);
    void muteToggled(Core::Snowflake userId, bool muted);
    void verificationCodeRequested(Core::Snowflake userId);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    Core::Snowflake userId;
    VoiceAvatar *avatarWidget;
    QLabel *nameLabel;
    VolumeMeter *meter;
    QSlider *volumeSlider;
    QPushButton *muteBtn;
    QLabel *stateLabel;
    QTimer speakingDecayTimer;
    bool locallyMuted = false;
    bool daveActive = false;

    static constexpr int SPEAKING_DECAY_MS = 300;
};

class VoiceWindow : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    explicit VoiceWindow(QWidget *parent = nullptr);

    void setVoiceManager(Core::AV::VoiceManager *manager);
    void setNameResolver(NameResolver resolver);
    void setAvatarResolver(AvatarResolver resolver);
    void setImageManager(Core::ImageManager *manager);
    void refreshDevices();

private:
    void setupUi();
    void disconnectManager();

    void onParticipantJoined(Core::Snowflake userId);
    void onParticipantLeft(Core::Snowflake userId);
    void onParticipantUpdated(Core::Snowflake userId);
    void onParticipantSpeakingChanged(Core::Snowflake userId, bool speaking);
    void onParticipantsCleared();
    void onUserAudioLevelChanged(Core::Snowflake userId, float rms);
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    void requestAvatar(Core::Snowflake userId, VoiceUserWidget *widget);
    void showVerificationCode(Core::Snowflake userId);
    void showPrivacyCode();

    Core::AV::VoiceManager *voiceManager = nullptr;
    Core::ImageManager *imageManager = nullptr;
    QMetaObject::Connection imageFetchedConn;
    NameResolver nameResolver;
    AvatarResolver avatarResolver;
    QHash<QUrl, Core::Snowflake> pendingAvatars;

    QScrollArea *userScrollArea;
    QWidget *userListContainer;
    QVBoxLayout *userListLayout;
    QHash<Core::Snowflake, VoiceUserWidget *> userWidgets;
    QPushButton *privacyCodeBtn;

    VolumeMeter *volumeMeter;
    QComboBox *inputDeviceCombo;
    QComboBox *outputDeviceCombo;
    QSlider *inputGainSlider;
    QSlider *outputVolumeSlider;
    QSlider *vadThresholdSlider;
    QLabel *inputGainValue;
    QLabel *outputVolumeValue;
    QLabel *vadThresholdValue;

    static constexpr QSize AVATAR_REQUEST_SIZE = QSize(32, 32);
};

} // namespace UI
} // namespace Acheron
