#pragma once

#include <QMultiHash>
#include <QPixmap>
#include <QSize>
#include <QUrl>

#include "Core/ImageManager.hpp"

namespace Acheron {
namespace UI {

template <typename Target>
class AvatarRequestTracker
{
public:
    QPixmap fetch(Core::ImageManager *imageManager, const QUrl &url, const QSize &size,
                  const Target &target, Core::Snowflake accountId,
                  Core::PinGroup pin = Core::PinGroup::None)
    {
        QPixmap pixmap = imageManager->get(url, size, accountId, pin);
        if (!imageManager->isCached(url, size))
            track(url, target);
        return pixmap;
    }

    void track(const QUrl &url, const Target &target)
    {
        auto it = pending.constFind(url);
        while (it != pending.cend() && it.key() == url) {
            if (it.value() == target)
                return;
            ++it;
        }
        pending.insert(url, target);
    }

    template <typename RefreshFn>
    void notify(const QUrl &url, RefreshFn &&refresh)
    {
        const auto targets = pending.values(url);
        if (targets.isEmpty())
            return;
        pending.remove(url);
        for (const Target &target : targets)
            refresh(target);
    }

    void clear() { pending.clear(); }

private:
    QMultiHash<QUrl, Target> pending;
};

} // namespace UI
} // namespace Acheron
