// Tests for Core::OrderedJson, the writer behind every gateway payload and the
// X-Super-Properties / X-Context-Properties headers. Expectations are the bytes JSON.stringify
// produces in the official client, so a drift here is a wire fingerprint, not a cosmetic diff.

#include "Core/JsonUtils.hpp"
#include "Core/OrderedJson.hpp"
#include "Discord/ContextProperties.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace Acheron;
using Core::OrderedJson;

class TestOrderedJson : public QObject
{
    Q_OBJECT
private slots:
    void testEmpty();
    void testInsertionOrder();
    void testScalars();
    void testNumbers();
    void testStringEscaping();
    void testNesting();
    void testQJsonArrayValue();
    void testMatchesQtWhenAlreadySorted();
    void testFieldInsert();
    void testContextProperties();
};

void TestOrderedJson::testEmpty()
{
    QCOMPARE(OrderedJson().toBytes(), QByteArray("{}"));
    QCOMPARE(OrderedJson::Array().toBytes(), QByteArray("[]"));
}

void TestOrderedJson::testInsertionOrder()
{
    OrderedJson obj;
    obj.insert("zeta", 1);
    obj.insert("alpha", 2);
    obj.insert("mid", 3);
    QCOMPARE(obj.toBytes(), QByteArray(R"({"zeta":1,"alpha":2,"mid":3})"));
}

void TestOrderedJson::testScalars()
{
    OrderedJson obj;
    obj.insert("t", true);
    obj.insert("f", false);
    obj.insert("n", QJsonValue::Null);
    obj.insert("s", "text");
    QCOMPARE(obj.toBytes(), QByteArray(R"({"t":true,"f":false,"n":null,"s":"text"})"));
}

void TestOrderedJson::testNumbers()
{
    OrderedJson obj;
    obj.insert("int", 42);
    obj.insert("neg", -7);
    obj.insert("wholeDouble", 2.0);
    obj.insert("negZero", -0.0);
    obj.insert("fraction", 1.5);
    obj.insert("big", static_cast<qint64>(1700000000000LL));
    obj.insert("nan", std::nan(""));
    obj.insert("inf", INFINITY);
    QCOMPARE(obj.toBytes(),
             QByteArray(R"({"int":42,"neg":-7,"wholeDouble":2,"negZero":0,"fraction":1.5,)"
                        R"("big":1700000000000,"nan":null,"inf":null})"));
}

void TestOrderedJson::testStringEscaping()
{
    OrderedJson obj;
    obj.insert("q", "say \"hi\"");
    obj.insert("bs", "a\\b");
    obj.insert("ctl", QString("tab\tnl\ncr\rbs\bff\f"));
    obj.insert("low", QString(QChar(0x01)) + QChar(0x1f));
    obj.insert("slash", "a/b");
    obj.insert("uni", QString::fromUtf8("caf\xC3\xA9 \xF0\x9F\x98\x80"));
    obj.insert(QString::fromUtf8("k\xC3\xA9y"), 1);
    // outside QCOMPARE: Visual Studio 2022 cant stringize a raw string containing \" in a macro argument
    const QByteArray expected = QByteArray(R"({"q":"say \"hi\"","bs":"a\\b","ctl":"tab\tnl\ncr\rbs\bff\f",)"
                                           R"("low":"\u0001\u001f","slash":"a/b","uni":"caf)") +
                                QByteArray("\xC3\xA9 \xF0\x9F\x98\x80") + QByteArray(R"(","k)") + QByteArray("\xC3\xA9") + QByteArray(R"(y":1})");
    QCOMPARE(obj.toBytes(), expected);
}

void TestOrderedJson::testNesting()
{
    OrderedJson inner;
    inner.insert("b", 1);
    inner.insert("a", 2);

    OrderedJson::Array pairs;
    pairs.append(OrderedJson::Array().append(0).append(99));
    pairs.append(OrderedJson::Array().append(100).append(199));

    OrderedJson::Array objects;
    objects.append(inner);

    OrderedJson obj;
    obj.insert("inner", inner);
    obj.insert("pairs", pairs);
    obj.insert("objects", objects);
    obj.insert("empty", OrderedJson());
    QCOMPARE(obj.toBytes(),
             QByteArray(R"({"inner":{"b":1,"a":2},"pairs":[[0,99],[100,199]],"objects":[{"b":1,"a":2}],"empty":{}})"));
}

void TestOrderedJson::testQJsonArrayValue()
{
    OrderedJson obj;
    obj.insert("activities", QJsonArray());
    obj.insert("mixed", QJsonArray{ 1, "two", QJsonValue::Null, true });
    QCOMPARE(obj.toBytes(), QByteArray(R"({"activities":[],"mixed":[1,"two",null,true]})"));
}

void TestOrderedJson::testMatchesQtWhenAlreadySorted()
{
    QJsonObject qt;
    qt["a"] = QString::fromUtf8("q\"uote\\ \n \xC3\xA9");
    qt["b"] = 1234567890123LL;
    qt["c"] = 0.1;
    qt["d"] = QJsonArray{ 1, 2.5, "x" };
    qt["e"] = QJsonValue::Null;
    qt["f"] = false;

    OrderedJson ordered;
    for (auto it = qt.constBegin(); it != qt.constEnd(); ++it)
        ordered.insert(it.key(), it.value());

    QCOMPARE(ordered.toBytes(), QJsonDocument(qt).toJson(QJsonDocument::Compact));
}

namespace {

struct Inner : Core::JsonUtils::JsonObject
{
    Field<QString> name;
    Field<int> value;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "name", name);
        insert(obj, "value", value);
        return obj;
    }
};

struct Outer : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QList<Core::Snowflake>> ids;
    Field<QString, true> absent;
    Field<QString, false, true> nulled;
    Field<Inner> inner;
    Field<QList<Inner>> inners;
    Field<QMap<Core::Snowflake, Inner>> byId;
    Field<QByteArray> bytes;
    Field<quint32> ssrc;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "id", id);
        insert(obj, "ids", ids);
        insert(obj, "absent", absent);
        insert(obj, "nulled", nulled);
        insert(obj, "inner", inner);
        insert(obj, "inners", inners);
        insert(obj, "by_id", byId);
        insert(obj, "bytes", bytes);
        insert(obj, "ssrc", ssrc);
        return obj;
    }
};

} // namespace

void TestOrderedJson::testFieldInsert()
{
    Inner one;
    one.name = "one";
    one.value = 1;
    Inner two;
    two.name = "two";
    two.value = 2;

    Outer outer;
    outer.id = Core::Snowflake(1234567890123456789ULL);
    outer.ids = QList<Core::Snowflake>{ Core::Snowflake(1), Core::Snowflake(2) };
    outer.nulled = nullptr;
    outer.inner = one;
    outer.inners = QList<Inner>{ one, two };
    outer.byId = QMap<Core::Snowflake, Inner>{ { Core::Snowflake(7), two } };
    outer.bytes = QByteArray("\x00\xff", 2);
    outer.ssrc = 4000000000u;

    QCOMPARE(outer.toJson().toBytes(),
             QByteArray(R"({"id":"1234567890123456789","ids":["1","2"],"nulled":null,)"
                        R"("inner":{"name":"one","value":1},)"
                        R"("inners":[{"name":"one","value":1},{"name":"two","value":2}],)"
                        R"("by_id":{"7":{"name":"two","value":2}},"bytes":[0,255],"ssrc":4000000000})"));
}

void TestOrderedJson::testContextProperties()
{
    QCOMPARE(Discord::ContextProperties::empty().toHeaderValue(), QByteArray("e30="));
    QCOMPARE(Discord::ContextProperties::location("chat_input").toHeaderValue(),
             QByteArray("eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="));

    Discord::ContextProperties invite = Discord::ContextProperties::location("Join Guild");
    invite.add("location_guild_id", "1").add("location_channel_type", 0);
    QCOMPARE(QByteArray::fromBase64(invite.toHeaderValue()),
             QByteArray(R"({"location":"Join Guild","location_guild_id":"1","location_channel_type":0})"));
}

QTEST_GUILESS_MAIN(TestOrderedJson)
#include "tst_OrderedJson.moc"
