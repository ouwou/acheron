#include "IdleMonitor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QPointer>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#elif defined(ACHERON_IDLE_DBUS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace Acheron {
namespace UI {

IdleMonitor::IdleMonitor(QObject *parent) : QObject(parent)
{
    lastInputMs = QDateTime::currentMSecsSinceEpoch();
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(ACHERON_IDLE_DBUS)
    useWindowActivity();
#endif
}

IdleMonitor::~IdleMonitor()
{
    if (watchingWindows)
        QCoreApplication::instance()->removeEventFilter(this);
}

void IdleMonitor::useWindowActivity()
{
    if (watchingWindows)
        return;
    watchingWindows = true;
    lastInputMs = QDateTime::currentMSecsSinceEpoch();
    QCoreApplication::instance()->installEventFilter(this);
}

bool IdleMonitor::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        lastInputMs = QDateTime::currentMSecsSinceEpoch();
        break;
    default:
        break;
    }
    return false;
}

qint64 IdleMonitor::windowIdleMs() const
{
    return QDateTime::currentMSecsSinceEpoch() - lastInputMs;
}

void IdleMonitor::query(std::function<void(qint64 idleMs)> callback)
{
#if defined(Q_OS_WIN)
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (GetLastInputInfo(&info)) {
        callback(qint64(DWORD(GetTickCount() - info.dwTime))); // wraps correctly every 49.7 days
        return;
    }
#elif defined(Q_OS_MACOS)
    const double seconds = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (seconds >= 0) {
        callback(qint64(seconds * 1000));
        return;
    }
#elif defined(ACHERON_IDLE_DBUS)
    if (!watchingWindows) {
        // GNOME (X11 or Wayland) answers in ms; other desktops through the screensaver service, in seconds
        const QDBusMessage mutter = QDBusMessage::createMethodCall(QStringLiteral("org.gnome.Mutter.IdleMonitor"),
                                                                   QStringLiteral("/org/gnome/Mutter/IdleMonitor/Core"),
                                                                   QStringLiteral("org.gnome.Mutter.IdleMonitor"),
                                                                   QStringLiteral("GetIdletime"));
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(mutter), this);
        const QPointer<IdleMonitor> self(this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [self, callback](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            if (!self)
                return;
            const QDBusPendingReply<qulonglong> reply = *w;
            if (!reply.isError()) {
                callback(qint64(reply.value()));
                return;
            }
            const QDBusMessage screensaver = QDBusMessage::createMethodCall(
                    QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver"),
                    QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("GetSessionIdleTime"));
            auto *second = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(screensaver), self);
            QObject::connect(second, &QDBusPendingCallWatcher::finished, self, [self, callback](QDBusPendingCallWatcher *w2) {
                w2->deleteLater();
                if (!self)
                    return;
                const QDBusPendingReply<uint> seconds = *w2;
                if (!seconds.isError()) {
                    callback(qint64(seconds.value()) * 1000);
                    return;
                }
                self->useWindowActivity(); // neither desktop service: from now on, judge by Acheron's windows
                callback(self->windowIdleMs());
            });
        });
        return;
    }
#endif
    if (!watchingWindows)
        useWindowActivity();
    callback(windowIdleMs());
}

} // namespace UI
} // namespace Acheron
