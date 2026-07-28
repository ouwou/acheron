#include "Core/Video/PlayerPool.hpp"

#include "Core/Logging.hpp"
#include "Core/Video/Player.hpp"

namespace Acheron {
namespace Core {
namespace Video {

QString attachmentKey(quint64 attachmentId)
{
    return QStringLiteral("att:%1").arg(attachmentId);
}

QString embedKey(quint64 messageId, int embedIndex)
{
    return QStringLiteral("embed:%1:%2").arg(messageId).arg(embedIndex);
}

PlayerPool::PlayerPool(QObject *parent) : QObject(parent) {}

PlayerPool::~PlayerPool()
{
    clear();
}

Player *PlayerPool::find(const QString &key) const
{
    auto it = players.constFind(key);
    return it != players.constEnd() ? it.value() : nullptr;
}

Player *PlayerPool::acquire(const QString &key, const QUrl &url)
{
    if (!isSupported() || key.isEmpty() || url.isEmpty())
        return nullptr;

    if (Player *existing = find(key)) {
        touch(key);
        return existing;
    }

    enforceLimit();

    auto *player = new Player(this);
    players.insert(key, player);
    order.append(key);

    connect(player, &Player::frameReady, this, [this, key] { emit frameReady(key); });
    connect(player, &Player::stateChanged, this, [this, key] { emit playerStateChanged(key); });
    connect(player, &Player::errorOccurred, this, [key](const QString &message) {
        qCWarning(LogVideo) << "player" << key << "failed:" << message;
    });

    player->open(url);
    return player;
}

void PlayerPool::touch(const QString &key)
{
    if (!players.contains(key))
        return;

    order.removeAll(key);
    order.append(key);
}

void PlayerPool::setPinned(const QString &key)
{
    pinnedKey = key;
}

void PlayerPool::release(const QString &key)
{
    auto it = players.find(key);
    if (it == players.end())
        return;

    Player *player = it.value();
    players.erase(it);
    order.removeAll(key);
    if (pinnedKey == key)
        pinnedKey.clear();

    player->close();
    player->deleteLater();

    emit playerReleased(key);
}

void PlayerPool::clear()
{
    const QStringList keys = players.keys();
    for (const QString &key : keys)
        release(key);
}

QString PlayerPool::evictionCandidate() const
{
    QString playingFallback;

    for (const QString &key : order) {
        if (key == pinnedKey)
            continue;

        const Player *player = find(key);
        if (!player)
            return key;

        if (!player->isPlaying())
            return key;

        if (playingFallback.isEmpty())
            playingFallback = key;
    }

    return playingFallback;
}

void PlayerPool::enforceLimit()
{
    while (players.size() >= MaxActivePlayers) {
        const QString victim = evictionCandidate();
        if (victim.isEmpty())
            return;

        qCDebug(LogVideo) << "evicting player" << victim << "to stay under the decoder cap";
        release(victim);
        order.removeAll(victim);
    }
}

} // namespace Video
} // namespace Core
} // namespace Acheron
