#pragma once

#include <QPixmap>
#include <QString>

namespace Acheron {
namespace UI {
namespace EmojiGlyphs {

[[nodiscard]] QPixmap pixmap(const QString &surrogates, int px, qreal devicePixelRatio);

} // namespace EmojiGlyphs
} // namespace UI
} // namespace Acheron
