#pragma once

#include <QCoreApplication>
#include <QMenu>
#include <QPointer>

#include <functional>

#include "Core/Streams/StreamWatcher.hpp"
#include "Core/Video/CallVideo.hpp"

namespace Acheron {
namespace UI {

// the same entries wherever a call's video is offered: in the channel list and on its tiles
inline QString callVideoText(const char *text)
{
    return QCoreApplication::translate("Acheron::UI::CallVideoMenu", text);
}

inline void addStreamActions(QMenu *menu, QPointer<Core::StreamWatcher> watcher, const QString &streamKey, std::function<void()> watch)
{
    if (const Core::StreamSession *session = watcher ? watcher->session(streamKey) : nullptr) {
        QObject::connect(menu->addAction(session->isOver() ? callVideoText("Close Stream") : callVideoText("Stop Watching")), &QAction::triggered, menu, [watcher, streamKey] {
            if (watcher)
                watcher->stopWatching(streamKey);
        });
        return;
    }
    QObject::connect(menu->addAction(callVideoText("Watch Stream")), &QAction::triggered, menu, std::move(watch));
}

inline void addDisableVideoAction(QMenu *menu, QPointer<Core::CallVideo> video, Core::Snowflake userId)
{
    QAction *disable = menu->addAction(callVideoText("Disable Video"));
    disable->setCheckable(true);
    disable->setChecked(video && video->isVideoDisabled(userId));
    QObject::connect(disable, &QAction::toggled, menu, [video, userId](bool disabled) {
        if (video)
            video->setVideoDisabled(userId, disabled);
    });
}

} // namespace UI
} // namespace Acheron
