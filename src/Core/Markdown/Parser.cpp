#include "Parser.hpp"

#include <QRegularExpression>

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
            if (foundMatch && rule.order > currentBestOrder)
                break;

            Capture capture;

            if (rule.match) {
                capture = rule.match(source, state);
            } else {
                capture = rule.regex.match(source, 0, QRegularExpression::NormalMatch,
                                           QRegularExpression::AnchoredMatchOption);
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

QString Parser::toHtml(const QList<AstNode> &nodes)
{
    QString result;
    for (const auto &node : nodes) {
        if (ruleMap.contains(node.type) && ruleMap[node.type]->html) {
            auto renderChildren = [this](const QList<AstNode> &children) {
                return this->toHtml(children);
            };
            result += ruleMap[node.type]->html(node, renderChildren);
        } else {
            result += node.content;
        }
    }
    return result;
}

static MatchFn inlineRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        if (state.isInline)
            return regex.match(source, 0, QRegularExpression::NormalMatch,
                               QRegularExpression::AnchoredMatchOption);
        else
            return Capture();
    };
}

static MatchFn blockRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        if (!state.isInline)
            return regex.match(source, 0, QRegularExpression::NormalMatch,
                               QRegularExpression::AnchoredMatchOption);
        else
            return Capture();
    };
}

static MatchFn anyScopeRegex(QRegularExpression regex)
{
    return [regex](const QString &source, const ParseState &state) -> Capture {
        return regex.match(source, 0, QRegularExpression::NormalMatch,
                           QRegularExpression::AnchoredMatchOption);
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