#pragma once

namespace Acheron {
namespace Core {

struct ChannelReadState
{
    bool isUnread = false;
    int mentionCount = 0;
    bool isMuted = false;
    bool countsForGuildUnread = false;
    // the official channel row shows an unread channel bright only when its unread setting is all messages or it has a mention
    bool unreadIsImportant = true;

    bool operator==(const ChannelReadState &) const = default;
};

} // namespace Core
} // namespace Acheron
