#pragma once

#include <QString>

#include <memory>
#include <vector>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

struct ChannelNode
{
    enum class Type {
        Root,
        Account,
        DMHeader,
        Folder,
        Server,
        Category,
        Channel,
        VoiceChannel,
        DMChannel,
    };

    Core::Snowflake id;
    QString name;
    Type type;
    int position = 0;
    bool isUnread = false;
    int mentionCount = 0;
    bool isMuted = false;
    bool collapsed = false;
    Core::Snowflake parentId;
    QString TEMP_iconHash;
    std::optional<QString> folderName;
    std::optional<uint64_t> folderColor;
    QList<Core::Snowflake> recipientIds;
    QString dmAvatarHash;
    Core::Snowflake dmRecipientId;
    Core::Snowflake lastMessageId;
    bool isPrivate = false;

    std::vector<std::unique_ptr<ChannelNode>> children;
    ChannelNode *parent = nullptr;

    ChannelNode *addChild(std::unique_ptr<ChannelNode> node)
    {
        node->parent = this;
        children.push_back(std::move(node));
        return children.back().get();
    }
};

} // namespace UI
} // namespace Acheron
