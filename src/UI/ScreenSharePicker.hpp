#pragma once

#include <QList>

#include "Core/Video/CaptureSources.hpp"
#include "Core/Video/SendQuality.hpp"
#include "Discord/Voice/VideoFrame.hpp"
#include "UI/Dialogs/BasePopup.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace Acheron {
namespace UI {

class ScreenSharePicker : public BasePopup
{
    Q_OBJECT
public:
    explicit ScreenSharePicker(const Core::Video::SharerTier &tier, QWidget *parent = nullptr);

    [[nodiscard]] static Core::Video::SendQuality rememberedQuality(const Core::Video::SharerTier &tier);
    static void rememberQuality(const Core::Video::SendQuality &quality);

    [[nodiscard]] static QString heightLabel(int height);
    [[nodiscard]] static QString framerateLabel(int framerate);

    static constexpr QSize THUMBNAIL_SIZE = QSize(168, 94);

signals:
    void shareRequested(const Acheron::Core::Video::CaptureSource &source, const Acheron::Core::Video::SendQuality &quality, bool withAudio);

private slots:
    void onRefreshClicked();
    void onScreenSelected();
    void onWindowSelected();
    void onHeightChosen(int index);
    void onFramerateChosen(int index);
    void onMethodChosen(int index);
    void onAudioToggled(bool checked);
    void onShareClicked();
    void onEncodersKnown(const QList<Acheron::Discord::Voice::VideoCodec> &codecs);

private:
    [[nodiscard]] QListWidget *makeSourceList(int visibleRows);
    void listSources();
    void loadThumbnailsFromTheFirst();
    void loadNextThumbnail();
    [[nodiscard]] QListWidgetItem *chosenItem() const;
    [[nodiscard]] std::optional<Core::Video::CaptureSource> chosenSource() const;
    [[nodiscard]] QList<Core::Video::CaptureSource> offeredSources() const;
    void listPrograms();
    void onSourceChosen(const Core::Video::CaptureSource &chosen, const QString &failure, quint32 program, bool withAudio);
    void showQuality();
    void refreshControls();

    Core::Video::SharerTier tier;
    Core::Video::SendQuality quality;
    QList<Core::Video::CaptureSource> sources;
    QList<QListWidgetItem *> sourceItems;
    int thumbnailsLoaded = 0;
    bool thumbnailQueued = false;
    bool encodersKnown = false;
    bool canEncode = false;
    bool choosing = false;
    QString choosingFailure;

    QListWidget *screenList;
    QListWidget *windowList;
    QComboBox *heightCombo;
    QComboBox *framerateCombo;
    QComboBox *methodCombo;
    QCheckBox *audioCheck;
    QComboBox *programCombo;
    QLabel *noteLabel;
    QPushButton *shareButton;
};

} // namespace UI
} // namespace Acheron
