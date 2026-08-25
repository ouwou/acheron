#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>

#include <memory>

#include "Core/ProxyConfig.hpp"

namespace Acheron {
namespace Core {
namespace Media {

enum class MediaKind {
    Video,
    Audio,
};

bool isSupported();

bool canPlay(const QString &contentType, const QUrl &url);

bool canPlayAudio(const QString &contentType, const QUrl &url);

float storedVolume();
void setStoredVolume(float volume);
bool storedMuted();
void setStoredMuted(bool muted);

class Player : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Opening,
        Playing,
        Paused,
        Ended,
        Error,
    };
    Q_ENUM(State)

    explicit Player(QObject *parent = nullptr);
    ~Player() override;

    void open(const QUrl &url, const ProxyConfig &proxy);
    void close();

    void play();
    void pause();
    void togglePlayPause();
    void seek(qint64 positionMs);

    void setVolume(float volume);
    [[nodiscard]] float volume() const;
    void setMuted(bool muted);
    [[nodiscard]] bool isMuted() const;
    void setTargetSize(const QSize &size);

    [[nodiscard]] QImage currentFrame() const;
    [[nodiscard]] qint64 position() const;
    [[nodiscard]] qint64 duration() const;
    [[nodiscard]] qint64 bufferedPosition() const;
    [[nodiscard]] QSize nativeSize() const;
    [[nodiscard]] bool hasVideo() const;
    [[nodiscard]] State state() const;

    [[nodiscard]] bool isPlaying() const { return state() == State::Playing; }
    [[nodiscard]] bool isSeekable() const;

signals:
    void frameReady();
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void stateChanged(Acheron::Core::Media::Player::State state);
    void errorOccurred(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
