#include "Parser.hpp"
#include "Core/EmojiSegmenter.hpp"

#include <QRegularExpression>

using Acheron::Core::countUnicodeEmojisSegmented;

namespace Acheron {
namespace Core {
namespace Markdown {

Parser::Parser()
{
    setupDefaultRules();
    sortRules();
}

QList<AstNode> Parser::parse(QString source, ParseState state)
{
    QList<AstNode> result;

    source.replace(QRegularExpression(R"(\r\n?)"), "\n");
    source.replace("\t", "    ");

    if (!state.isInline) {
        source += "\n\n";
    }

    while (!source.isEmpty()) {
        MarkdownRule *bestRule = nullptr;
        Capture bestCapture;
        double bestQuality = std::numeric_limits<double>::lowest();

        int currentBestOrder = -1;
        bool foundMatch = false;

        for (auto &rule : rules) {
            if (state.excludedRules.contains(rule.name))
                continue;

            if (foundMatch && rule.order > currentBestOrder)
                break;

            Capture capture;

            if (rule.match) {
                capture = rule.match(source, state);
            } else {
                capture = rule.regex.match(source, 0, QRegularExpression::NormalMatch,
                                           QRegularExpression::AnchorAtOffsetMatchOption);
            }

            if (capture.hasMatch()) {
                double quality = 0.0;
                if (rule.quality) {
                    quality = rule.quality(capture, state, state.prevCapture);
                }

                if (!foundMatch || quality >= bestQuality) {
                    bestRule = &rule;
                    bestCapture = capture;
                    bestQuality = quality;
                    currentBestOrder = rule.order;
                    foundMatch = true;
                }
            }
        }

        if (!foundMatch) {
            // bad! error!
            AstNode node;
            node.type = "text";
            node.content = source.left(1);
            result.append(node);

            state.prevCapture = node.content;
            source.remove(0, 1);
            continue;
        }

        auto nestedParse = [this](QString source, ParseState state) -> QList<AstNode> {
            return this->parse(source, state);
        };

        AstNode parsedNode = bestRule->parse(bestCapture, nestedParse, state);

        if (parsedNode.type.isEmpty()) {
            parsedNode.type = bestRule->name;
        }

        // maybe flatten here
        result.append(parsedNode);

        QString capturedStr = bestCapture.captured(0);
        state.prevCapture = capturedStr;
        source.remove(0, capturedStr.length());
    }

    return result;
}

QString Parser::toHtml(const QList<AstNode> &nodes, bool jumboEmoji)
{
    return toHtmlInternal(nodes, jumboEmoji);
}

bool Parser::isEmojiOnly(const QList<AstNode> &nodes, int maxEmojis)
{
    int totalEmojis = 0;
    for (const auto &node : nodes) {
        if (node.type == "customEmoji") {
            totalEmojis++;
            if (totalEmojis > maxEmojis)
                return false;
            continue;
        }
        if (node.type == "br" || node.type == "newline")
            continue;
        if (node.type == "text" || node.type.isEmpty()) {
            int unicodeCount = countUnicodeEmojisSegmented(node.content);
            if (unicodeCount < 0)
                return false;
            totalEmojis += unicodeCount;
            if (totalEmojis > maxEmojis)
                return false;
            continue;
        }
        // Any other node type (em, strong, url, link, code, etc.) disqualifies
        return false;
    }
    return totalEmojis > 0;
}

QString Parser::toHtmlInternal(const QList<AstNode> &nodes, bool jumboEmoji)
{
    QString result;
    for (const auto &node : nodes) {
        // todo i dont really like this. i think this should still belong in MarkdownRule somehow. its ok for now
        if (node.type == "user") {
            QString displayName;
            if (userResolver)
                displayName = userResolver(node.content);
            else
                displayName = node.content;

            result += QString("<span style=\"background-color: rgba(88, 101, 242, 0.3); color: #c9cdfb;\">@%1</span>")
                              .arg(displayName.toHtmlEscaped());
            continue;
        }

        if (node.type == "channel") {
            QString channelName;
            if (channelResolver)
                channelName = channelResolver(node.content);
            else
                channelName = "#" + node.content;

            result += QString("<span style=\"color: #5865F2; font-weight: 500;\">#%1</span>")
                              .arg(channelName.toHtmlEscaped());
            continue;
        }

        if (node.type == "customEmoji") {
            QString id = node.content;
            QString name = node.attributes["name"].toString();
            int emojiSize = jumboEmoji ? 44 : 22;
            QString url = QString("https://cdn.discordapp.com/emojis/%1.webp?size=128").arg(id);
            result += QString(R"(<img src="%1" alt=":%2:" width="%3" height="%3" style="vertical-align: middle;" />)")
                              .arg(url.toHtmlEscaped())
                              .arg(name.toHtmlEscaped())
                              .arg(emojiSize);
            continue;
        }

        if (jumboEmoji && node.type == "text") {
            QString escaped = node.content.toHtmlEscaped();
            if (!node.content.trimmed().isEmpty())
                result += QString(R"(<span style="font-size: 44px;">%1</span>)").arg(escaped);
            else
                result += escaped;
            continue;
        }

        if (ruleMap.contains(node.type) && ruleMap[node.type]->html) {
            auto renderChildren = [this, jumboEmoji](const QList<AstNode> &children) {
                return this->toHtmlInternal(children, jumboEmoji);
            };
            result += ruleMap[node.type]->html(node, renderChildren);
        } else {
            result += node.content;
        }
    }
    return result;
}

void Parser::setUserResolver(UserResolverFn resolver)
{
    userResolver = std::move(resolver);
}

void Parser::setChannelResolver(ChannelResolverFn resolver)
{
    channelResolver = std::move(resolver);
}

static MatchFn inlineRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        if (state.isInline)
            return regex.match(source, 0, QRegularExpression::NormalMatch,
                               QRegularExpression::AnchorAtOffsetMatchOption);
        else
            return Capture();
    };
}

static MatchFn blockRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        if (!state.isInline)
            return regex.match(source, 0, QRegularExpression::NormalMatch,
                               QRegularExpression::AnchorAtOffsetMatchOption);
        else
            return Capture();
    };
}

static MatchFn anyScopeRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        return regex.match(source, 0, QRegularExpression::NormalMatch,
                           QRegularExpression::AnchorAtOffsetMatchOption);
    };
}

static MatchFn startOfLineRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        // In "inline" mode we still want to match patterns that start a new line
        // (Discord supports headers/subtext inside regular messages).
        if (!state.prevCapture.isEmpty() && !state.prevCapture.endsWith('\n'))
            return Capture();
        return regex.match(source, 0, QRegularExpression::NormalMatch,
                           QRegularExpression::AnchorAtOffsetMatchOption);
    };
}

void Parser::setupDefaultRules()
{
    MarkdownRule newline;
    newline.name = "newline";
    newline.order = 10;
    newline.regex = QRegularExpression(R"(^(?:\n *)*\n)");
    newline.match = blockRegex(newline.regex);
    newline.parse = [](const Capture &match, NestedParseFn nestedParse,
                       ParseState state) -> AstNode { return {}; };
    newline.html = [](const AstNode &node,
                      std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return "\n";
    };
    rules.append(newline);

    MarkdownRule escape;
    escape.name = "escape";
    escape.order = 12;
    escape.regex = QRegularExpression(R"(^\\([^0-9A-Za-z\s]))");
    escape.match = inlineRegex(escape.regex);
    escape.parse = [](const Capture &match, NestedParseFn nestedParse,
                      ParseState state) -> AstNode {
        AstNode node;
        node.type = "text";
        node.content = match.captured(1);
        return node;
    };
    escape.html = [](const AstNode &node,
                     std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return node.content.toHtmlEscaped();
    };
    rules.append(escape);

    MarkdownRule subtext;
    subtext.name = "subtext";
    subtext.order = 13;
    subtext.regex = QRegularExpression(R"(^(?:-#)[ \t]+([^\n]*)(?:\n|$))");
    subtext.match = startOfLineRegex(subtext.regex);
    subtext.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "subtext";
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(match.captured(1), childState);
        return node;
    };
    subtext.html = [](const AstNode &node,
                      std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString(R"(<span style="color:#b5bac1; font-size:10px;">%1</span><br>)")
                .arg(renderChildren(node.children));
    };
    rules.append(subtext);

    MarkdownRule header;
    header.name = "header";
    header.order = 14;
    header.regex = QRegularExpression(R"(^(#{1,3})[ \t]+([^\n]*)(?:\n|$))");
    header.match = startOfLineRegex(header.regex);
    header.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "header";
        node.attributes["level"] = match.captured(1).length();
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(match.captured(2), childState);
        return node;
    };
    header.html = [](const AstNode &node,
                     std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        int level = node.attributes.value("level").toInt();
        level = qBound(1, level, 3);
        // Use spans (Qt rich text can add extra paragraph spacing for block tags).
        int size = (level == 1) ? 20 : (level == 2 ? 18 : 16);
        int weight = 700;
        // Include a <br> because the rule consumes the newline.
        return QString(R"(<span style="font-size:%1px; font-weight:%2; line-height:1.25;">%3</span><br>)")
                .arg(size)
                .arg(weight)
                .arg(renderChildren(node.children));
    };
    rules.append(header);

    MarkdownRule url;
    url.name = "url";
    url.order = 16;
    url.regex = QRegularExpression(R"(^(https?:\/\/[^\s<]+[^<.,:;"')\]\s]))");
    url.match = inlineRegex(url.regex);
    url.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "url";
        node.content = match.captured(1);
        node.attributes["href"] = node.content;
        return node;
    };
    url.html = [](const AstNode &node,
                  std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<a href=\"%1\">%2</a>")
                .arg(node.attributes["href"].toString().toHtmlEscaped())
                .arg(node.content.toHtmlEscaped());
    };
    rules.append(url);

    // Autolink format: <https://example.com>
    // Discord and markdown often wrap naked links in angle brackets; we should not render the <>.
    MarkdownRule autolink;
    autolink.name = "autolink";
    autolink.order = 16; // same order group as url; will win on longer capture
    autolink.regex = QRegularExpression(R"(^(?:<)(https?:\/\/[^>\s]+)(?:>))");
    autolink.match = inlineRegex(autolink.regex);
    autolink.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "url";
        node.content = match.captured(1);
        node.attributes["href"] = node.content;
        return node;
    };
    autolink.html = url.html; // render identically to url
    autolink.quality = [](const Capture &match, const ParseState &, const QString &) -> double {
        return match.capturedLength() + 0.3; // prefer over plain text
    };
    rules.append(autolink);

    MarkdownRule link;
    link.name = "link";
    link.order = 17;
    link.regex = QRegularExpression(
            R"(^\[((?:\[[^\]]*\]|[^\[\]]|\](?=[^\[]*\]))*)\]\(\s*<?((?:\([^)]*\)|[^\s\\]|\\.)*?)>?(?:\s+['"]([\s\S]*?)['"])?\s*\))");
    link.match = inlineRegex(link.regex);
    link.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "link";
        node.children = nestedParse(match.captured(1), state);
        node.attributes["href"] = match.captured(2);
        node.attributes["title"] = match.captured(3);
        return node;
    };
    link.html = [](const AstNode &node,
                   std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<a href=\"%1\">%2</a>")
                .arg(node.attributes["href"].toString().toHtmlEscaped())
                .arg(renderChildren(node.children));
    };
    rules.append(link);

    MarkdownRule em;
    em.name = "em";
    em.order = 20;
    em.regex = QRegularExpression(
            R"(^\b_((?:__|\\[\s\S]|[^\\_])+?)_\b|^\*(?=\S)((?:\*\*|\\[\s\S]|\s+(?:\\[\s\S]|[^\s\*\\]|\*\*)|[^\s\*\\])+?)\*(?!\*))");
    em.match = inlineRegex(em.regex);
    em.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "em";
        QString innerContent = match.captured(2).isNull() ? match.captured(1) : match.captured(2);
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(innerContent, childState);
        return node;
    };
    em.html = [](const AstNode &node,
                 std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<em>%1</em>").arg(renderChildren(node.children));
    };
    em.quality = [](const Capture &match, const ParseState &state,
                    const QString &prevCapture) -> double { return match.capturedLength() + 0.2; };
    rules.append(em);

    MarkdownRule user;
    user.name = "user";
    user.order = 21;
    user.regex = QRegularExpression(R"(^<@!?([0-9]*)>)");
    user.match = anyScopeRegex(user.regex);
    user.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "user";
        node.content = match.captured(1);
        return node;
    };
    // .html handled in toHtml() because of user resolution
    rules.append(user);

    MarkdownRule channel;
    channel.name = "channel";
    channel.order = 21;
    channel.regex = QRegularExpression(R"(^<#([0-9]*)>)");
    channel.match = anyScopeRegex(channel.regex);
    channel.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "channel";
        node.content = match.captured(1);
        return node;
    };
    // .html handled in toHtml() because of channel resolution
    rules.append(channel);

    MarkdownRule customEmoji;
    customEmoji.name = "customEmoji";
    customEmoji.order = 21;
    customEmoji.regex = QRegularExpression(R"(^<(a?):([a-zA-Z0-9_]{1,32}):(\d+)>)");
    customEmoji.match = anyScopeRegex(customEmoji.regex);
    customEmoji.parse = [](const Capture &match, NestedParseFn nestedParse,
                           ParseState state) -> AstNode {
        AstNode node;
        node.type = "customEmoji";
        node.content = match.captured(3);
        node.attributes["name"] = match.captured(2);
        node.attributes["animated"] = !match.captured(1).isEmpty();
        return node;
    };
    // .html handled in toHtmlInternal() for jumbo emoji support
    rules.append(customEmoji);

    MarkdownRule strong;
    strong.name = "strong";
    strong.order = 21;
    strong.regex = QRegularExpression(R"(^\*\*((?:\\[\s\S]|[^\\])+?)\*\*(?!\*))");
    strong.match = inlineRegex(strong.regex);
    strong.parse = [](const Capture &match, NestedParseFn nestedParse,
                      ParseState state) -> AstNode {
        AstNode node;
        node.type = "strong";
        QString innerContent = match.captured(1);
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(innerContent, childState);
        return node;
    };
    strong.html = [](const AstNode &node,
                     std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<strong>%1</strong>").arg(renderChildren(node.children));
    };
    strong.quality = [](const Capture &match, const ParseState &state,
                        const QString &prevCapture) -> double {
        return match.capturedLength() + 0.1;
    };
    rules.append(strong);

    MarkdownRule u;
    u.name = "u";
    u.order = 21;
    u.regex = QRegularExpression(R"(^__((?:\\[\s\S]|[^\\])+?)__(?!_))");
    u.match = inlineRegex(u.regex);
    u.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.type = "u";
        QString innerContent = match.captured(1);
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(innerContent, childState);
        return node;
    };
    u.html = [](const AstNode &node,
                std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<u>%1</u>").arg(renderChildren(node.children));
    };
    u.quality = [](const Capture &match, const ParseState &state,
                   const QString &prevCapture) -> double { return match.capturedLength(); };
    rules.append(u);

    MarkdownRule strike;
    strike.name = "strike";
    strike.order = 22;
    strike.regex = QRegularExpression(R"(~~([\s\S]+?)~~(?!_))");
    strike.match = inlineRegex(strike.regex);
    strike.parse = [](const Capture &match, NestedParseFn nestedParse,
                      ParseState state) -> AstNode {
        AstNode node;
        node.type = "strike";
        QString innerContent = match.captured(1);
        ParseState childState = state;
        childState.isInline = true;
        node.children = nestedParse(innerContent, childState);
        return node;
    };
    strike.html = [](const AstNode &node,
                     std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<s>%1</s>").arg(renderChildren(node.children));
    };
    rules.append(strike);

    MarkdownRule inlineCode;
    inlineCode.name = "inlineCode";
    inlineCode.order = 23;
    inlineCode.regex = QRegularExpression(R"(^(`+)([\s\S]*?[^`])\1(?!`))");
    inlineCode.match = inlineRegex(inlineCode.regex);
    inlineCode.parse = [](const Capture &match, NestedParseFn nestedParse,
                          ParseState state) -> AstNode {
        AstNode node;
        node.type = "inlineCode";
        node.content = match.captured(2);
        static QRegularExpression re(R"(^ (?= *`)|(` *) $)");
        node.content.replace(re, "\\1");
        return node;
    };
    inlineCode.html = [](const AstNode &node,
                         std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return QString("<code>%1</code>").arg(node.content.toHtmlEscaped());
    };
    rules.append(inlineCode);

    MarkdownRule br;
    br.name = "br";
    br.order = 24;
    br.regex = QRegularExpression(R"(^\n)");
    br.match = anyScopeRegex(br.regex);
    br.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        return {};
    };
    br.html = [](const AstNode &node,
                 std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return "<br>";
    };
    rules.append(br);

    MarkdownRule text;
    text.name = "text";
    text.order = 25;
    text.regex =
            QRegularExpression(R"(^[\s\S]+?(?=[^0-9A-Za-z\s\x{00C0}-\x{ffff}-]|\n\n|\n|\w+:\S|$))");
    text.match = anyScopeRegex(text.regex);
    text.parse = [](const Capture &match, NestedParseFn nestedParse, ParseState state) -> AstNode {
        AstNode node;
        node.content = match.captured(0);
        return node;
    };
    text.html = [](const AstNode &node,
                   std::function<QString(const QList<AstNode> &)> renderChildren) -> QString {
        return node.content.toHtmlEscaped();
    };
    rules.append(text);
}

void Parser::sortRules()
{
    std::sort(rules.begin(), rules.end(),
              [](const MarkdownRule &a, const MarkdownRule &b) { return a.order < b.order; });

    for (auto &rule : rules) {
        ruleMap[rule.name] = &rule;
    }
}

} // namespace Markdown
} // namespace Core
} // namespace Acheron