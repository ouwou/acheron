#pragma once

#include <QString>

#include "Core/MessageReactors.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
class ClientInstance;
}
namespace UI {

[[nodiscard]] QString reactionEmojiName(const Discord::Emoji &emoji);

[[nodiscard]] QString reactionTooltip(Core::ClientInstance *instance, Core::Snowflake channelId, Core::Snowflake guildId, const Core::ReactionRef &reaction, int reactionCount);

} // namespace UI
} // namespace Acheron
