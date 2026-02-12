#pragma once

#include <QRegularExpression>
#include <QVariantMap>

namespace Acheron {
namespace Core {
namespace Markdown {

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

class Parser
{
public:
    Parser();

    QList<AstNode> parse(QString source, ParseState state = {});
    QString toHtml(const QList<AstNode> &nodes, bool jumboEmoji = false);

    void setUserResolver(UserResolverFn resolver);

    static bool isEmojiOnly(const QList<AstNode> &nodes, int maxEmojis = 30);

private:
    void setupDefaultRules();
    void sortRules();

    QString toHtmlInternal(const QList<AstNode> &nodes, bool jumboEmoji);

private:
    QList<MarkdownRule> rules;
    QMap<QString, MarkdownRule *> ruleMap;
    UserResolverFn userResolver;
};

} // namespace Markdown
} // namespace Core
} // namespace Acheron
