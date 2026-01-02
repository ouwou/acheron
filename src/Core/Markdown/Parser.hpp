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

class Parser
{
public:
    Parser();

    QList<AstNode> parse(QString source, ParseState state = {});
    QString toHtml(const QList<AstNode> &nodes);

private:
    void setupDefaultRules();
    void sortRules();

private:
    QList<MarkdownRule> rules;
    QMap<QString, MarkdownRule *> ruleMap;
};

} // namespace Markdown
} // namespace Core
} // namespace Acheron
