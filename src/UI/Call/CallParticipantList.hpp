#pragma once

#include <QPointer>
#include <QScrollArea>
#include <QUrl>

#include <functional>
#include <map>

#include "Core/Snowflake.hpp"
#include "UI/AvatarRequestTracker.hpp"

class QVBoxLayout;

namespace Acheron {
namespace Core {
class ImageManager;
namespace Audio {
class VoiceManager;
}
} // namespace Core

namespace UI {

class CallParticipantRow;

class CallParticipantList : public QScrollArea
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    CallParticipantList(Core::Audio::VoiceManager *voice, Core::ImageManager *images, Core::Snowflake accountId, NameResolver names, AvatarResolver avatars, QWidget *parent);
    ~CallParticipantList() override;

    void refresh();
    [[nodiscard]] int wantedHeight() const;

    static constexpr int MOST_ROWS_SHOWN = 8;

signals:
    void menuRequested(Core::Snowflake userId, const QPoint &globalPos);

private:
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);
    [[nodiscard]] CallParticipantRow *rowOf(Core::Snowflake userId) const;

    QPointer<Core::Audio::VoiceManager> voice;
    Core::ImageManager *images;
    Core::Snowflake accountId;
    NameResolver names;
    AvatarResolver avatars;
    AvatarRequestTracker<Core::Snowflake> avatarTracker;
    QVBoxLayout *rowsLayout;
    std::map<Core::Snowflake, CallParticipantRow *> rows;
};

} // namespace UI
} // namespace Acheron
