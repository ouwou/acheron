#pragma once

#include <QString>

#include <memory>
#include <vector>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

struct ChannelNode
{
    enum class Type { Root, Account, DMHeader, Server, Category, Channel };
    Core::Snowflake id;
    QString name;
    Type type;
    int unreadCount = 5;
    int position = 0;
    QString TEMP_iconHash;

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
