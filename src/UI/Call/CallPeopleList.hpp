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

class CallPersonRow;

class CallPeopleList : public QScrollArea
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    CallPeopleList(Core::Audio::VoiceManager *voice, Core::ImageManager *images, Core::Snowflake accountId, NameResolver names, AvatarResolver avatars, QWidget *parent);
    ~CallPeopleList() override;

    void refresh();
    [[nodiscard]] int wantedHeight() const;

    static constexpr int MOST_ROWS_SHOWN = 8;

signals:
    void menuRequested(Core::Snowflake userId, const QPoint &globalPos);

private:
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);
    [[nodiscard]] CallPersonRow *rowOf(Core::Snowflake userId) const;

    QPointer<Core::Audio::VoiceManager> voice;
    Core::ImageManager *images;
    Core::Snowflake accountId;
    NameResolver names;
    AvatarResolver avatars;
    AvatarRequestTracker<Core::Snowflake> avatarTracker;
    QVBoxLayout *rowsLayout;
    std::map<Core::Snowflake, CallPersonRow *> rows;
};

} // namespace UI
} // namespace Acheron
