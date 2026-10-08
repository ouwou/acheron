#include "Core/Gifs/FavoriteGifs.hpp"

#include <QPointer>

#include <algorithm>

#include "Core/Logging.hpp"
#include "Discord/Client.hpp"
#include "Proto/ProtoReader.hpp"

namespace Acheron {
namespace Core {

namespace {

constexpr int RateLimitedRetryMs = 30 * 1000;

} // namespace

FavoriteGifs::FavoriteGifs(Discord::Client *client, QObject *parent) : QObject(parent), client(client)
{
    retryTimer.setSingleShot(true);
    retryTimer.setInterval(RateLimitedRetryMs);
    connect(&retryTimer, &QTimer::timeout, this, &FavoriteGifs::save);
}

QList<Proto::FavoriteGif> FavoriteGifs::newestFirst() const
{
    return FavoriteGifRules::newestFirst(favorites.gifs);
}

int FavoriteGifs::indexOf(const QString &key) const
{
    const auto found = std::find_if(favorites.gifs.cbegin(), favorites.gifs.cend(), [&key](const Proto::FavoriteGif &gif) { return gif.url == key; });
    return found == favorites.gifs.cend() ? -1 : int(found - favorites.gifs.cbegin());
}

bool FavoriteGifs::contains(const QString &url) const
{
    return indexOf(url) >= 0 || indexOf(FavoriteGifRules::keyFor(url)) >= 0;
}

void FavoriteGifs::add(const FavoriteGifCandidate &candidate)
{
    if (!loaded || !candidate.isValid())
        return;

    const Proto::FavoriteGif gif = FavoriteGifRules::stored(candidate, FavoriteGifRules::orderAfter(favorites.gifs));

    Proto::FavoriteGifs edited = favorites;
    const int existing = indexOf(gif.url);
    if (existing >= 0)
        edited.gifs[existing] = gif;
    else
        edited.gifs.append(gif);
    if (edited.gifs.size() > FavoriteGifRules::CountThatHidesTheTooltip)
        edited.hideTooltip = true;

    if (edited.toProto().size() > FavoriteGifRules::OfficialClientMaxSerializedBytes) {
        emit limitReached();
        return;
    }

    favorites = edited;
    emit changed();
    saveSoon();
}

void FavoriteGifs::remove(const QString &url)
{
    if (!loaded)
        return;

    const int exact = indexOf(url);
    const int index = exact >= 0 ? exact : indexOf(FavoriteGifRules::keyFor(url));
    if (index < 0)
        return;

    favorites.gifs.removeAt(index);
    emit changed();
    saveSoon();
}

void FavoriteGifs::toggle(const FavoriteGifCandidate &candidate)
{
    if (contains(candidate.url))
        remove(candidate.url);
    else
        add(candidate);
}

void FavoriteGifs::onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings)
{
    loaded = true;
    if (unsavedEdits)
        return;
    replaceWith(settings.favoriteGifs.value_or(Proto::FavoriteGifs()));
}

void FavoriteGifs::onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event)
{
    if (!event.type.hasValue() || event.type.get() != Discord::UserSettingsProtoType::FRECENCY || !event.proto.hasValue())
        return;

    const QByteArray decoded = QByteArray::fromBase64(event.proto.get().toUtf8());
    if (decoded.isEmpty() || unsavedEdits)
        return;

    Proto::ProtoReader reader(decoded);
    const Proto::FrecencyUserSettings settings = Proto::FrecencyUserSettings::fromProto(reader);

    const bool partial = event.partial.hasValue() && event.partial.get();
    if (settings.favoriteGifs)
        replaceWith(*settings.favoriteGifs);
    else if (!partial)
        replaceWith(Proto::FavoriteGifs());
}

void FavoriteGifs::replaceWith(const Proto::FavoriteGifs &fromServer)
{
    favorites = fromServer;
    emit changed();
}

void FavoriteGifs::saveSoon()
{
    unsavedEdits = true;
    if (!saveInFlight && !retryTimer.isActive())
        save();
}

void FavoriteGifs::save()
{
    Proto::FrecencyUserSettings onlyFavorites;
    onlyFavorites.favoriteGifs = favorites;

    unsavedEdits = false;
    saveInFlight = true;

    QPointer<FavoriteGifs> self(this);
    client->patchFrecencySettings(onlyFavorites.toProtoPartial(), std::nullopt, [this, self](const Discord::Client::FrecencyPatchResult &result) {
        if (!self)
            return;

        saveInFlight = false;

        if (result.rateLimited) {
            qCWarning(LogCore) << "FavoriteGifs: save rate limited, retrying";
            unsavedEdits = true;
            retryTimer.start();
            return;
        }

        if (unsavedEdits) {
            save();
            return;
        }

        if (!result.success) {
            qCWarning(LogCore) << "FavoriteGifs: save failed, reloading:" << result.error;
            reloadFromServer();
        }
    });
}

void FavoriteGifs::reloadFromServer()
{
    client->fetchFrecencySettings([](const Result<Proto::FrecencyUserSettings> &) {});
}

} // namespace Core
} // namespace Acheron
