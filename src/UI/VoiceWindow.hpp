#pragma once

#include <QWidget>
#include <QLabel>
#include <QSlider>
#include <QComboBox>
#include <QList>

#include "Discord/AV/VoiceClient.hpp"

namespace Acheron {
namespace Core {
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
    float threshold = 100.0f;

    static constexpr float MAX_RMS = 2000.0f;
};

class VoiceWindow : public QWidget
{
    Q_OBJECT
public:
    explicit VoiceWindow(QWidget *parent = nullptr);

    void setVoiceManager(Core::AV::VoiceManager *manager);
    void refreshDevices();

private:
    void setupUi();
    void disconnectManager();

    Core::AV::VoiceManager *voiceManager = nullptr;
    QList<QMetaObject::Connection> managerConnections;

    VolumeMeter *volumeMeter;
    QComboBox *inputDeviceCombo;
    QComboBox *outputDeviceCombo;
    QSlider *inputGainSlider;
    QSlider *outputVolumeSlider;
    QSlider *vadThresholdSlider;
    QLabel *inputGainValue;
    QLabel *outputVolumeValue;
    QLabel *vadThresholdValue;
};

} // namespace UI
} // namespace Acheron
