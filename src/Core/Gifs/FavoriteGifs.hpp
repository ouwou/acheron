#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include "Core/Gifs/FavoriteGifRules.hpp"
#include "Discord/Events.hpp"
#include "Proto/FrecencySettings.hpp"

namespace Acheron {

namespace Discord {
class Client;
}

namespace Core {

class FavoriteGifs : public QObject
{
    Q_OBJECT
public:
    explicit FavoriteGifs(Discord::Client *client, QObject *parent = nullptr);

    [[nodiscard]] bool isLoaded() const { return loaded; }
    [[nodiscard]] QList<Proto::FavoriteGif> newestFirst() const;
    [[nodiscard]] bool contains(const QString &url) const;

    void add(const FavoriteGifCandidate &candidate);
    void remove(const QString &url);
    void toggle(const FavoriteGifCandidate &candidate);

public slots:
    void onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings);
    void onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event);

signals:
    void changed();
    void limitReached();

private:
    [[nodiscard]] int indexOf(const QString &key) const;
    void replaceWith(const Proto::FavoriteGifs &fromServer);
    void saveSoon();
    void save();
    void reloadFromServer();

    Discord::Client *client;
    Proto::FavoriteGifs favorites;
    QTimer retryTimer;
    bool loaded = false;
    bool unsavedEdits = false;
    bool saveInFlight = false;
};

} // namespace Core
} // namespace Acheron
