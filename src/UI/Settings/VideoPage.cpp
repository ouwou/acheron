#include "VideoPage.hpp"

#include "Core/Media/CodecSupport.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Streams/StreamPreviews.hpp"
#include "Core/Video/CallVideo.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "UI/CaptureMethodCombo.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace Acheron {
namespace UI {

namespace {

using Core::Video::CaptureMethod;
using Discord::Voice::VideoCodec;

constexpr int MOST_DECODERS = 25;

QHBoxLayout *labelledRow(QVBoxLayout *layout, const QString &label, QWidget *control)
{
    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(label, control->parentWidget()));
    row->addStretch(1);
    row->addWidget(control);
    layout->addLayout(row);
    return row;
}

} // namespace

VideoPage::VideoPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    // both fall back by themselves when the machine cant; these are for when it can but should not
    auto *hardwareDecode = new QCheckBox(tr("Decode video with the graphics card"), this);
    hardwareDecode->setChecked(Core::Media::hardwareDecodingEnabled());
    layout->addWidget(hardwareDecode);
    connect(hardwareDecode, &QCheckBox::toggled, this, [](bool enabled) { Core::Media::setHardwareDecodingEnabled(enabled); });

    auto *gpuDrawing = new QCheckBox(tr("Draw video with the graphics card"), this);
    gpuDrawing->setChecked(VideoSurface::gpuDrawingEnabled());
    layout->addWidget(gpuDrawing);
    connect(gpuDrawing, &QCheckBox::toggled, this, [](bool enabled) { VideoSurface::setGpuDrawingEnabled(enabled); });

    auto *maxDecoders = new QSpinBox(this);
    maxDecoders->setRange(1, MOST_DECODERS);
    maxDecoders->setValue(std::min(Core::CallVideo::maxDecoders(), MOST_DECODERS));
    labelledRow(layout, tr("Videos decoded at once"), maxDecoders);
    connect(maxDecoders, qOverload<int>(&QSpinBox::valueChanged), this, [](int decoders) { Core::CallVideo::setMaxDecoders(decoders); });

    auto *previews = new QCheckBox(tr("Send previews of what I share"), this);
    previews->setChecked(Core::StreamPreviewUploader::enabled());
    layout->addWidget(previews);
    connect(previews, &QCheckBox::toggled, this, [](bool enabled) { Core::StreamPreviewUploader::setEnabled(enabled); });

    QComboBox *captureMethod = makeCaptureMethodCombo(this);
    labelledRow(layout, tr("Capture Method"), captureMethod);
    connect(captureMethod, qOverload<int>(&QComboBox::activated), this, [captureMethod] { Core::Video::setCaptureMethod(chosenCaptureMethod(captureMethod)); });

    if (Core::Video::cameraCaptureAvailable()) {
        const QList<Core::Video::CameraDevice> cameras = Core::Video::cameraDevices();
        auto *camera = new QComboBox(this);
        camera->addItem(tr("First available"), QString());
        for (const Core::Video::CameraDevice &device : cameras)
            camera->addItem(device.name, device.id);
        camera->setCurrentIndex(std::max(0, camera->findData(Core::Video::preferredCamera(cameras))));
        labelledRow(layout, tr("Camera"), camera);
        connect(camera, qOverload<int>(&QComboBox::activated), this, [camera](int index) {
            Core::Video::setPreferredCamera(camera->itemData(index).toString());
        });

        auto *secondLayer = new QCheckBox(tr("Send the camera in two layers"), this);
        secondLayer->setChecked(Core::Video::cameraSecondLayerEnabled());
        layout->addWidget(secondLayer);
        connect(secondLayer, &QCheckBox::toggled, this, [](bool enabled) { Core::Video::setCameraSecondLayerEnabled(enabled); });
    }

    auto *codecs = new QHBoxLayout();
    codecs->addWidget(new QLabel(tr("Send with"), this));
    codecs->addStretch(1);
    for (const VideoCodec codec : Discord::Voice::VIDEO_CODECS_BEST_FIRST) {
        auto *check = new QCheckBox(Discord::Voice::videoCodecName(codec), this);
        check->setChecked(Core::Media::sendCodecAllowed(codec));
        codecs->addWidget(check);
        sendCodecChecks.append(check);
        connect(check, &QCheckBox::toggled, this, [this, check, codec](bool allowed) {
            const bool othersLeft = std::any_of(sendCodecChecks.cbegin(), sendCodecChecks.cend(), [](const QCheckBox *each) { return each->isChecked(); });
            if (!allowed && !othersLeft) {
                check->setChecked(true);
                return;
            }
            Core::Media::setSendCodecAllowed(codec, allowed);
        });
    }
    layout->addLayout(codecs);

    auto *note = new QLabel(tr("Video settings apply to streams and cameras opened afterwards."), this);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    layout->addWidget(note);

    layout->addStretch(1);
}

} // namespace UI
} // namespace Acheron
