#include "VoiceWindow.hpp"

#include "Core/AV/VoiceManager.hpp"

#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLinearGradient>

namespace Acheron {
namespace UI {

VolumeMeter::VolumeMeter(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(12);
}

void VolumeMeter::setLevel(float rms)
{
    level = rms;
    update();
}

void VolumeMeter::setThreshold(float t)
{
    threshold = t;
    update();
}

void VolumeMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), QColor(0x0d, 0x0d, 0x15));

    float fraction = qBound(0.0f, level / MAX_RMS, 1.0f);
    int barWidth = static_cast<int>(fraction * width());

    if (barWidth > 0) {
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0.0, QColor(0x43, 0xb5, 0x81));
        gradient.setColorAt(0.5, QColor(0xfa, 0xa6, 0x1a));
        gradient.setColorAt(1.0, QColor(0xf0, 0x47, 0x47));

        p.fillRect(0, 0, barWidth, height(), gradient);
    }

    float threshFraction = qBound(0.0f, threshold / MAX_RMS, 1.0f);
    int threshX = static_cast<int>(threshFraction * width());
    p.setPen(QPen(Qt::white, 1));
    p.drawLine(threshX, 0, threshX, height());
}

VoiceWindow::VoiceWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Voice Settings"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi();
}

void VoiceWindow::setupUi()
{
    setFixedWidth(280);

    setStyleSheet("QWidget { background-color: #1a1a2e; }"
                  "QLabel { color: #999; font-size: 11px; background: transparent; }"
                  "QSlider::groove:horizontal { background: #2a2a3e; height: 4px; border-radius: 2px; }"
                  "QSlider::handle:horizontal { background: #7289da; width: 12px; margin: -4px 0; border-radius: 6px; }"
                  "QSlider::sub-page:horizontal { background: #7289da; border-radius: 2px; }"
                  "QComboBox { background: #2a2a3e; color: #ccc; border: 1px solid #3a3a4e; border-radius: 3px; padding: 2px 6px; font-size: 11px; }"
                  "QComboBox::drop-down { border: none; }"
                  "QComboBox QAbstractItemView { background: #2a2a3e; color: #ccc; selection-background-color: #7289da; border: 1px solid #3a3a4e; }");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    volumeMeter = new VolumeMeter(this);
    layout->addWidget(volumeMeter);

    auto *inputDevRow = new QHBoxLayout;
    inputDevRow->setSpacing(4);
    auto *inputDevLabel = new QLabel(tr("Input:"), this);
    inputDevLabel->setFixedWidth(44);
    inputDeviceCombo = new QComboBox(this);
    inputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    inputDevRow->addWidget(inputDevLabel);
    inputDevRow->addWidget(inputDeviceCombo, 1);
    layout->addLayout(inputDevRow);

    auto *outputDevRow = new QHBoxLayout;
    outputDevRow->setSpacing(4);
    auto *outputDevLabel = new QLabel(tr("Output:"), this);
    outputDevLabel->setFixedWidth(44);
    outputDeviceCombo = new QComboBox(this);
    outputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    outputDevRow->addWidget(outputDevLabel);
    outputDevRow->addWidget(outputDeviceCombo, 1);
    layout->addLayout(outputDevRow);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(4);
    auto *inputLabel = new QLabel(tr("Input"), this);
    inputLabel->setFixedWidth(44);
    inputGainSlider = new QSlider(Qt::Horizontal, this);
    inputGainSlider->setRange(0, 200);
    inputGainSlider->setValue(100);
    inputGainValue = new QLabel("100%", this);
    inputGainValue->setFixedWidth(36);
    inputGainValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(inputGainSlider, 1);
    inputRow->addWidget(inputGainValue);
    layout->addLayout(inputRow);

    auto *outputRow = new QHBoxLayout;
    outputRow->setSpacing(4);
    auto *outputLabel = new QLabel(tr("Output"), this);
    outputLabel->setFixedWidth(44);
    outputVolumeSlider = new QSlider(Qt::Horizontal, this);
    outputVolumeSlider->setRange(0, 200);
    outputVolumeSlider->setValue(100);
    outputVolumeValue = new QLabel("100%", this);
    outputVolumeValue->setFixedWidth(36);
    outputVolumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outputRow->addWidget(outputLabel);
    outputRow->addWidget(outputVolumeSlider, 1);
    outputRow->addWidget(outputVolumeValue);
    layout->addLayout(outputRow);

    auto *vadRow = new QHBoxLayout;
    vadRow->setSpacing(4);
    auto *vadLabel = new QLabel(tr("VAD"), this);
    vadLabel->setFixedWidth(44);
    vadThresholdSlider = new QSlider(Qt::Horizontal, this);
    vadThresholdSlider->setRange(0, 2000);
    vadThresholdSlider->setValue(100);
    vadThresholdValue = new QLabel("100", this);
    vadThresholdValue->setFixedWidth(36);
    vadThresholdValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vadRow->addWidget(vadLabel);
    vadRow->addWidget(vadThresholdSlider, 1);
    vadRow->addWidget(vadThresholdValue);
    layout->addLayout(vadRow);

    connect(inputGainSlider, &QSlider::valueChanged, this, [this](int value) {
        inputGainValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setInputGain(static_cast<float>(value) / 100.0f);
    });

    connect(outputVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        outputVolumeValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setOutputVolume(static_cast<float>(value) / 100.0f);
    });

    connect(vadThresholdSlider, &QSlider::valueChanged, this, [this](int value) {
        vadThresholdValue->setText(QString::number(value));
        volumeMeter->setThreshold(static_cast<float>(value));
        if (voiceManager)
            voiceManager->setVadThreshold(static_cast<float>(value));
    });

    connect(inputDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = inputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setInputDevice(deviceId);
    });

    connect(outputDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = outputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setOutputDevice(deviceId);
    });
}

void VoiceWindow::setVoiceManager(Core::AV::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    if (!voiceManager)
        return;

    managerConnections.append(
            connect(voiceManager, &Core::AV::VoiceManager::audioLevelChanged,
                    volumeMeter, &VolumeMeter::setLevel));

    managerConnections.append(
            connect(voiceManager, &Core::AV::VoiceManager::devicesChanged,
                    this, &VoiceWindow::refreshDevices));

    refreshDevices();
}

void VoiceWindow::refreshDevices()
{
    if (!voiceManager)
        return;

    inputDeviceCombo->blockSignals(true);
    inputDeviceCombo->clear();
    const auto inputs = voiceManager->availableInputDevices();
    QByteArray currentInput = voiceManager->currentInputDevice();
    int inputIndex = 0;
    for (int i = 0; i < inputs.size(); i++) {
        const auto &dev = inputs[i];
        QString label = dev.description;
        if (dev.isDefault)
            label += tr(" (Default)");
        inputDeviceCombo->addItem(label, dev.id);
        if (!currentInput.isEmpty() && dev.id == currentInput)
            inputIndex = i;
        else if (currentInput.isEmpty() && dev.isDefault) // doesnt get reported in time
            inputIndex = i;
    }
    inputDeviceCombo->setCurrentIndex(inputIndex);
    inputDeviceCombo->blockSignals(false);

    outputDeviceCombo->blockSignals(true);
    outputDeviceCombo->clear();
    const auto outputs = voiceManager->availableOutputDevices();
    QByteArray currentOutput = voiceManager->currentOutputDevice();
    int outputIndex = 0;
    for (int i = 0; i < outputs.size(); i++) {
        const auto &dev = outputs[i];
        QString label = dev.description;
        if (dev.isDefault)
            label += tr(" (Default)");
        outputDeviceCombo->addItem(label, dev.id);
        if (!currentOutput.isEmpty() && dev.id == currentOutput)
            outputIndex = i;
        else if (currentOutput.isEmpty() && dev.isDefault)
            outputIndex = i;
    }
    outputDeviceCombo->setCurrentIndex(outputIndex);
    outputDeviceCombo->blockSignals(false);
}

void VoiceWindow::disconnectManager()
{
    for (auto &conn : managerConnections)
        QObject::disconnect(conn);
    managerConnections.clear();
    voiceManager = nullptr;
}

} // namespace UI
} // namespace Acheron
