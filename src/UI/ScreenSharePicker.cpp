#include "ScreenSharePicker.hpp"

#include "Core/Audio/LoopbackCapture.hpp"
#include "Core/Media/CodecSupport.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "UI/CaptureMethodCombo.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace Acheron {
namespace UI {

using Core::Video::CaptureMethod;
using Core::Video::CaptureSource;
using Core::Video::SendQuality;

namespace {

const char *const HEIGHT_KEY = "video/share_height";
const char *const FRAMERATE_KEY = "video/share_framerate";
const char *const AUDIO_KEY = "video/share_audio";

constexpr int SOURCE_COLUMNS = 3;
constexpr int SOURCE_GAP = 12;
constexpr int SOURCE_LABEL_HEIGHT = 22;
constexpr int VISIBLE_WINDOW_ROWS = 2;

void setItemEnabled(QComboBox *combo, int index, bool enabled)
{
    if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
        model->item(index)->setEnabled(enabled);
}

} // namespace

ScreenSharePicker::ScreenSharePicker(const Core::Video::SharerTier &tier, QWidget *parent)
    : BasePopup(parent), tier(tier), quality(rememberedQuality(tier))
{
    auto *layout = new QVBoxLayout(getContainer());
    layout->setSpacing(8);
    layout->setContentsMargins(16, 16, 16, 16);

    auto *titleRow = new QHBoxLayout;
    auto *titleLabel = new QLabel(tr("Share Your Screen"), getContainer());
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel->setFont(titleFont);
    titleRow->addWidget(titleLabel, 1);
    auto *refreshButton = new QToolButton(getContainer());
    refreshButton->setText(tr("Refresh"));
    refreshButton->setToolTip(tr("List the screens and windows again"));
    refreshButton->setAutoRaise(true);
    titleRow->addWidget(refreshButton);
    layout->addLayout(titleRow);
    connect(refreshButton, &QToolButton::clicked, this, &ScreenSharePicker::onRefreshClicked);

    const bool windowsOffered = Core::Video::windowCaptureAvailable();
    auto *screensLabel = new QLabel(tr("Screens"), getContainer());
    screensLabel->setVisible(windowsOffered);
    layout->addWidget(screensLabel);
    screenList = makeSourceList(windowsOffered ? 1 : VISIBLE_WINDOW_ROWS);
    layout->addWidget(screenList);
    auto *windowsLabel = new QLabel(tr("Windows"), getContainer());
    windowsLabel->setVisible(windowsOffered);
    layout->addWidget(windowsLabel);
    windowList = makeSourceList(VISIBLE_WINDOW_ROWS);
    windowList->setVisible(windowsOffered);
    layout->addWidget(windowList);
    connect(screenList, &QListWidget::itemSelectionChanged, this, &ScreenSharePicker::onScreenSelected);
    connect(windowList, &QListWidget::itemSelectionChanged, this, &ScreenSharePicker::onWindowSelected);

    auto *optionsRow = new QHBoxLayout;
    optionsRow->addWidget(new QLabel(tr("Resolution"), getContainer()));
    heightCombo = new QComboBox(getContainer());
    for (const int height : Core::Video::SEND_HEIGHTS)
        heightCombo->addItem(heightLabel(height), height);
    optionsRow->addWidget(heightCombo);
    optionsRow->addSpacing(12);
    optionsRow->addWidget(new QLabel(tr("Frame Rate"), getContainer()));
    framerateCombo = new QComboBox(getContainer());
    for (const int framerate : Core::Video::SEND_FRAMERATES)
        framerateCombo->addItem(framerateLabel(framerate), framerate);
    optionsRow->addWidget(framerateCombo);
    optionsRow->addSpacing(12);
    optionsRow->addWidget(new QLabel(tr("Capture Method"), getContainer()));
    methodCombo = makeCaptureMethodCombo(getContainer());
    optionsRow->addWidget(methodCombo);
    optionsRow->addStretch(1);
    layout->addLayout(optionsRow);
    connect(heightCombo, QOverload<int>::of(&QComboBox::activated), this, &ScreenSharePicker::onHeightChosen);
    connect(framerateCombo, QOverload<int>::of(&QComboBox::activated), this, &ScreenSharePicker::onFramerateChosen);
    connect(methodCombo, QOverload<int>::of(&QComboBox::activated), this, &ScreenSharePicker::onMethodChosen);

    auto *audioRow = new QHBoxLayout;
    audioCheck = new QCheckBox(tr("Share audio"), getContainer());
    audioRow->addWidget(audioCheck);
    programCombo = new QComboBox(getContainer());
    audioRow->addWidget(programCombo);
    audioRow->addStretch(1);
    layout->addLayout(audioRow);
    connect(audioCheck, &QCheckBox::toggled, this, &ScreenSharePicker::onAudioToggled);

    noteLabel = new QLabel(getContainer());
    noteLabel->setWordWrap(true);
    layout->addWidget(noteLabel);

    auto *buttons = new QDialogButtonBox(getContainer());
    shareButton = buttons->addButton(tr("Share"), QDialogButtonBox::AcceptRole);
    shareButton->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ScreenSharePicker::onShareClicked);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const bool soundCapturable = Core::Audio::processLoopbackAvailable();
    audioCheck->setEnabled(soundCapturable);
    audioCheck->setChecked(soundCapturable && QSettings().value(AUDIO_KEY, true).toBool());

    listSources();
    showQuality();
    Core::Media::whenSendCodecsKnown(this, [this](const QList<Discord::Voice::VideoCodec> &codecs) { onEncodersKnown(codecs); });
}

SendQuality ScreenSharePicker::rememberedQuality(const Core::Video::SharerTier &tier)
{
    const SendQuality standard;
    QSettings settings;
    const SendQuality remembered{ settings.value(HEIGHT_KEY, standard.height).toInt(), settings.value(FRAMERATE_KEY, standard.framerate).toInt() };
    return Core::Video::usableQuality(remembered, tier);
}

void ScreenSharePicker::rememberQuality(const SendQuality &quality)
{
    QSettings settings;
    settings.setValue(HEIGHT_KEY, quality.height);
    settings.setValue(FRAMERATE_KEY, quality.framerate);
}

QString ScreenSharePicker::heightLabel(int height)
{
    return height == SendQuality::SOURCE_HEIGHT ? tr("Source") : tr("%1p").arg(height);
}

QString ScreenSharePicker::framerateLabel(int framerate)
{
    return tr("%1 fps").arg(framerate);
}

QListWidget *ScreenSharePicker::makeSourceList(int visibleRows)
{
    auto *list = new QListWidget(getContainer());
    list->setViewMode(QListView::IconMode);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setIconSize(THUMBNAIL_SIZE);
    list->setGridSize(THUMBNAIL_SIZE + QSize(SOURCE_GAP, SOURCE_LABEL_HEIGHT));
    list->setUniformItemSizes(true);
    list->setWordWrap(false);
    list->setTextElideMode(Qt::ElideRight);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMinimumWidth(list->gridSize().width() * SOURCE_COLUMNS + list->verticalScrollBar()->sizeHint().width() + 2 * list->frameWidth());
    list->setFixedHeight(list->gridSize().height() * visibleRows + 2 * list->frameWidth());
    connect(list, &QListWidget::itemActivated, this, &ScreenSharePicker::onShareClicked);
    return list;
}

void ScreenSharePicker::listSources()
{
    const std::optional<CaptureSource> chosen = chosenSource();
    const QString chosenId = chosen ? chosen->id : QString();

    {
        const QSignalBlocker screensBlocked(screenList);
        const QSignalBlocker windowsBlocked(windowList);
        screenList->clear();
        windowList->clear();
        sourceItems.clear();
        sources = offeredSources();

        QPixmap blank(THUMBNAIL_SIZE);
        blank.fill(Qt::black);
        for (int i = 0; i < sources.size(); i++) {
            const CaptureSource &source = sources[i];
            const bool isWindow = source.kind == CaptureSource::Kind::Window;
            const QString label = isWindow || source.geometry.isEmpty() ? source.name : tr("%1 (%2x%3)").arg(source.name).arg(source.geometry.width()).arg(source.geometry.height());
            auto *item = new QListWidgetItem(QIcon(blank), label, isWindow ? windowList : screenList);
            item->setData(Qt::UserRole, i);
            item->setToolTip(label);
            item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
            item->setSelected(source.id == chosenId);
            sourceItems.append(item);
        }
        if (!chosenItem() && !sourceItems.isEmpty())
            sourceItems.first()->setSelected(true);
    }

    listPrograms();
    refreshControls();
    loadThumbnailsFromTheFirst();
}

QList<CaptureSource> ScreenSharePicker::offeredSources() const
{
    const CaptureMethod method = chosenCaptureMethod(methodCombo);
    return Core::Video::captureSources(method) + Core::Video::captureWindows(method);
}

void ScreenSharePicker::listPrograms()
{
    programCombo->clear();
    programCombo->addItem(tr("Everything but Acheron"), 0U);
    for (const Core::Audio::PlayingProgram &program : Core::Audio::playingPrograms())
        programCombo->addItem(program.name, program.processId);
}

// one still per event loop turn, so the picker stays usable while a window is slow to draw
void ScreenSharePicker::loadThumbnailsFromTheFirst()
{
    thumbnailsLoaded = 0;
    if (std::exchange(thumbnailQueued, true))
        return;
    QTimer::singleShot(0, this, &ScreenSharePicker::loadNextThumbnail);
}

void ScreenSharePicker::loadNextThumbnail()
{
    thumbnailQueued = false;
    if (thumbnailsLoaded >= sources.size())
        return;

    const int index = thumbnailsLoaded++;
    const QImage still = Core::Video::captureThumbnail(sources[index], THUMBNAIL_SIZE * devicePixelRatioF());
    if (!still.isNull()) {
        QPixmap thumbnail(THUMBNAIL_SIZE * devicePixelRatioF());
        thumbnail.fill(Qt::black);
        const QImage fitted = still.scaled(thumbnail.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&thumbnail);
        painter.drawImage((thumbnail.width() - fitted.width()) / 2, (thumbnail.height() - fitted.height()) / 2, fitted);
        painter.end();
        thumbnail.setDevicePixelRatio(devicePixelRatioF());
        sourceItems[index]->setIcon(QIcon(thumbnail));
    }
    thumbnailQueued = true;
    QTimer::singleShot(0, this, &ScreenSharePicker::loadNextThumbnail);
}

QListWidgetItem *ScreenSharePicker::chosenItem() const
{
    for (const QListWidget *list : { screenList, windowList }) {
        const QList<QListWidgetItem *> selected = list->selectedItems();
        if (!selected.isEmpty())
            return selected.first();
    }
    return nullptr;
}

std::optional<CaptureSource> ScreenSharePicker::chosenSource() const
{
    const QListWidgetItem *chosen = chosenItem();
    if (!chosen)
        return std::nullopt;
    return sources[chosen->data(Qt::UserRole).toInt()];
}

void ScreenSharePicker::onRefreshClicked()
{
    listSources();
}

void ScreenSharePicker::onScreenSelected()
{
    if (!screenList->selectedItems().isEmpty())
        windowList->clearSelection();
    refreshControls();
}

void ScreenSharePicker::onWindowSelected()
{
    if (!windowList->selectedItems().isEmpty())
        screenList->clearSelection();
    refreshControls();
}

void ScreenSharePicker::showQuality()
{
    for (int i = 0; i < heightCombo->count(); i++)
        setItemEnabled(heightCombo, i, Core::Video::mayUseQuality(Core::Video::atHeight(quality, heightCombo->itemData(i).toInt(), tier), tier));
    for (int i = 0; i < framerateCombo->count(); i++)
        setItemEnabled(framerateCombo, i, Core::Video::mayUseQuality({ quality.height, framerateCombo->itemData(i).toInt() }, tier));

    heightCombo->setCurrentIndex(heightCombo->findData(quality.height));
    framerateCombo->setCurrentIndex(framerateCombo->findData(quality.framerate));
}

void ScreenSharePicker::onHeightChosen(int index)
{
    quality = Core::Video::atHeight(quality, heightCombo->itemData(index).toInt(), tier);
    showQuality();
}

void ScreenSharePicker::onFramerateChosen(int index)
{
    quality.framerate = framerateCombo->itemData(index).toInt();
}

void ScreenSharePicker::onMethodChosen(int)
{
    const QList<CaptureSource> offered = offeredSources();
    const auto sameSource = [](const CaptureSource &one, const CaptureSource &other) { return one.id == other.id; };
    if (!std::equal(offered.begin(), offered.end(), sources.begin(), sources.end(), sameSource))
        listSources();
}

void ScreenSharePicker::onAudioToggled(bool)
{
    refreshControls();
}

void ScreenSharePicker::onEncodersKnown(const QList<Discord::Voice::VideoCodec> &codecs)
{
    encodersKnown = true;
    canEncode = !codecs.isEmpty();
    refreshControls();
}

void ScreenSharePicker::refreshControls()
{
    if (!choosingFailure.isEmpty())
        noteLabel->setText(choosingFailure);
    else if (sources.isEmpty())
        noteLabel->setText(tr("No screen can be captured on this machine."));
    else if (!encodersKnown)
        noteLabel->setText(tr("Checking this machine's video encoders..."));
    else if (!canEncode)
        noteLabel->setText(tr("This machine has no video encoder that Discord accepts, so it cannot share a screen."));
    else
        noteLabel->clear();
    noteLabel->setVisible(!noteLabel->text().isEmpty());

    const std::optional<CaptureSource> chosen = chosenSource();
    const CaptureSource source = chosen.value_or(CaptureSource());
    const bool ofUnknownProgram = source.kind == CaptureSource::Kind::Window && Core::Video::isChosenByTheDesktop(source);
    programCombo->setVisible(ofUnknownProgram && audioCheck->isEnabled());
    programCombo->setEnabled(audioCheck->isChecked());

    shareButton->setEnabled(canEncode && chosen && !choosing);
}

void ScreenSharePicker::onShareClicked()
{
    const std::optional<CaptureSource> chosen = chosenSource();
    if (!canEncode || !chosen || choosing)
        return;

    rememberQuality(quality);
    Core::Video::setCaptureMethod(chosenCaptureMethod(methodCombo));
    if (audioCheck->isEnabled())
        QSettings().setValue(AUDIO_KEY, audioCheck->isChecked());

    const quint32 program = programCombo->isVisibleTo(this) ? programCombo->currentData().toUInt() : 0;
    const bool withAudio = audioCheck->isEnabled() && audioCheck->isChecked();
    choosing = true;
    choosingFailure.clear();
    refreshControls();
    Core::Video::chooseSource(*chosen, this, [this, program, withAudio](const CaptureSource &source, const QString &failure) { onSourceChosen(source, failure, program, withAudio); });
}

void ScreenSharePicker::onSourceChosen(const CaptureSource &chosen, const QString &failure, quint32 program, bool withAudio)
{
    choosing = false;
    if (chosen.id.isEmpty()) {
        choosingFailure = failure;
        refreshControls();
        return;
    }

    CaptureSource source = chosen;
    if (Core::Video::isHandedOverByTheDesktop(source))
        source.processId = program;
    emit shareRequested(source, quality, withAudio);
    accept();
}

} // namespace UI
} // namespace Acheron
