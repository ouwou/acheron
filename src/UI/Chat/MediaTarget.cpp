#include "UI/Chat/MediaTarget.hpp"

#include "Core/Media/PlayerPool.hpp"
#include "UI/Chat/ChatModel.hpp"

namespace Acheron {
namespace UI {
namespace MediaTargets {

using Core::Media::MediaKind;

std::optional<MediaTarget> forAttachment(const AttachmentData &attachment, const QRect &rect, const ChatModel *model)
{
    if (!Core::Media::isSupported() || !(attachment.isVideo || attachment.isAudio))
        return std::nullopt;

    MediaTarget target;
    target.kind = attachment.isAudio ? MediaKind::Audio : MediaKind::Video;
    target.key = Core::Media::attachmentKey(attachment.id, target.kind);
    target.url = attachment.originalUrl.isEmpty() ? attachment.proxyUrl : attachment.originalUrl;
    target.rect = rect;
    target.attachmentId = attachment.id;
    target.voiceMessage = attachment.isVoiceMessage;
    target.spoilered = attachment.isSpoiler && model && !model->isSpoilerRevealed(attachment.id);
    target.durationMs = attachment.durationMs;
    target.poster = attachment.pixmap;
    target.uploadSent = attachment.uploadSent;
    target.uploadTotal = attachment.uploadTotal;

    if (target.url.isEmpty())
        return std::nullopt;

    return target;
}

std::optional<MediaTarget> forEmbed(const EmbedData &embed, Core::Snowflake messageId, int embedIndex, const QRect &rect)
{
    if (!Core::Media::isSupported() || !ChatLayout::embedHasPlayableVideo(embed))
        return std::nullopt;

    MediaTarget target;
    target.key = Core::Media::embedKey(messageId, embedIndex);
    target.url = embed.videoUrl;
    target.rect = rect;
    target.poster = embed.videoThumbnail;

    if (target.url.isEmpty())
        return std::nullopt;

    return target;
}

MediaTarget forRegion(const ChatLayout::ResolvedLayout &resolved, const ChatLayout::HitRegion &region)
{
    using Kind = ChatLayout::HitRegion::Kind;

    if (region.kind == Kind::AttachmentVideo || region.kind == Kind::AttachmentAudio) {
        if (region.index < 0 || region.index >= resolved.ctx.attachments.size())
            return MediaTarget();

        const AttachmentData &attachment = resolved.ctx.attachments[region.index];
        const bool wantAudio = region.kind == Kind::AttachmentAudio;
        if (wantAudio != attachment.isAudio)
            return MediaTarget();

        return forAttachment(attachment, region.rect, resolved.ctx.model).value_or(MediaTarget());
    }

    if (region.kind == Kind::EmbedVideoThumbnail) {
        if (region.index < 0 || region.index >= resolved.ctx.embeds.size())
            return MediaTarget();

        return forEmbed(resolved.ctx.embeds[region.index], resolved.ctx.messageId, region.index, region.rect)
                .value_or(MediaTarget());
    }

    return MediaTarget();
}

} // namespace MediaTargets
} // namespace UI
} // namespace Acheron
