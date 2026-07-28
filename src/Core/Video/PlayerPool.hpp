#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace Acheron {
namespace Core {
namespace Video {

class Player;

QString attachmentKey(quint64 attachmentId);
QString embedKey(quint64 messageId, int embedIndex);

class PlayerPool : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxActivePlayers = 3;

    explicit PlayerPool(QObject *parent = nullptr);
    ~PlayerPool() override;

    [[nodiscard]] Player *find(const QString &key) const;

    Player *acquire(const QString &key, const QUrl &url);

    void touch(const QString &key);
    void release(const QString &key);
    void clear();

    void setPinned(const QString &key);

signals:
    void frameReady(const QString &key);
    void playerStateChanged(const QString &key);
    void playerReleased(const QString &key);

private:
    void enforceLimit();
    [[nodiscard]] QString evictionCandidate() const;

    QHash<QString, Player *> players;
    QStringList order;
    QString pinnedKey;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
