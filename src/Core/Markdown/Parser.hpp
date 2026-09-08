#pragma once

#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
namespace Markdown {

inline constexpr int InlineEmojiPx = 22;
inline constexpr int JumboEmojiPx = 44;

struct AstNode
{
    QString type;
    QString content;
    QList<AstNode> children;
    QVariantMap attributes;
};

struct ParseState
{
    bool isInline = false;
    bool inTable = false;
    int key = 0;
    QString prevCapture = "";
    QVariantMap customState;
    QSet<QString> excludedRules;
};

using Capture = QRegularExpressionMatch;

// clang-format off
using NestedParseFn = std::function<QList<AstNode>(QString, ParseState)>;
using MatchFn = std::function<Capture(const QString &, const ParseState &)>;
using ParseFn = std::function<AstNode(const Capture &, NestedParseFn, ParseState)>;
using QualityFn = std::function<double(const Capture&, const ParseState&, const QString&)>;
using HtmlOutputFn = std::function<QString(const AstNode&, std::function<QString(const QList<AstNode>&)>)>;
// clang-format on

struct MarkdownRule
{
    QString name;
    int order;
    QRegularExpression regex;
    MatchFn match = nullptr;
    ParseFn parse;
    HtmlOutputFn html;
    QualityFn quality = nullptr;
};

using UserResolverFn = std::function<QString(const QString &userId)>;
using ChannelResolverFn = std::function<QString(const QString &channelId)>;

struct ChannelLinkRef
{
    Snowflake guildId; // invalid for DM links
    Snowflake channelId;
    Snowflake messageId; // invalid for plain channel links
    Snowflake sourceChannelId; // channel the message containing the link was posted in
};

struct ChannelLinkPart
{
    QString icon;
    QString text;
    bool italic = false;
};

using ChannelLinkResolverFn = std::function<QList<ChannelLinkPart>(const ChannelLinkRef &)>;

class Parser
{
public:
    Parser();

    QList<AstNode> parse(QString source, ParseState state = {});
    QString toHtml(const QList<AstNode> &nodes, bool jumboEmoji = false);

    void setUserResolver(UserResolverFn resolver);
    void setChannelResolver(ChannelResolverFn resolver);
    void setChannelLinkResolver(ChannelLinkResolverFn resolver);

    static bool isEmojiOnly(const QList<AstNode> &nodes, int maxEmojis = 30);

private:
    void setupDefaultRules();
    void sortRules();

    QString toHtmlInternal(const QList<AstNode> &nodes, bool jumboEmoji);

private:
    QList<MarkdownRule> rules;
    QMap<QString, MarkdownRule *> ruleMap;
    UserResolverFn userResolver;
    ChannelResolverFn channelResolver;
    ChannelLinkResolverFn channelLinkResolver;
};

} // namespace Markdown
} // namespace Core
} // namespace Acheron
