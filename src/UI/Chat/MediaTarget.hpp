#pragma once

#include <QList>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QUrl>

#include <optional>

#include "Core/Media/Player.hpp"
#include "Core/Snowflake.hpp"
#include "UI/Chat/ChatLayout.hpp"
#include "UI/Chat/VideoControls.hpp"

namespace Acheron {
namespace UI {

class ChatModel;

// one playable surface within a message
struct MediaTarget
{
    QString key;
    QUrl url;
    QRect rect;
    Core::Snowflake attachmentId;
    Core::Media::MediaKind kind = Core::Media::MediaKind::Video;
    bool voiceMessage = false;
    bool spoilered = false;
    qint64 durationMs = 0;

    QPixmap poster;
    qint64 uploadSent = -1;
    qint64 uploadTotal = -1;

    [[nodiscard]] bool isValid() const { return !url.isEmpty(); }
    [[nodiscard]] bool isAudio() const { return kind == Core::Media::MediaKind::Audio; }

    [[nodiscard]] VideoControls::MediaInfo info() const { return { isAudio(), voiceMessage, durationMs }; }
};

namespace MediaTargets {

std::optional<MediaTarget> forAttachment(const AttachmentData &attachment, const QRect &rect, const ChatModel *model);

std::optional<MediaTarget> forEmbed(const EmbedData &embed, Core::Snowflake messageId, int embedIndex, const QRect &rect);

MediaTarget forRegion(const ChatLayout::ResolvedLayout &resolved, const ChatLayout::HitRegion &region);

} // namespace MediaTargets

} // namespace UI
} // namespace Acheron
