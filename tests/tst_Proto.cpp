// Tests for the FrecencyUserSettings wire format (settings-proto type 2). The
// encoding is hand-rolled on both sides, so the risks are: a map entry written
// in a shape the official client cannot read, a negative int32 mangled by the
// varint path, and an unknown field derailing the parse of the fields we do
// want.

#include "Proto/FrecencySettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/ProtoWriter.hpp"

#include <QTest>

using namespace Acheron::Proto;

class TestProto : public QObject
{
    Q_OBJECT
private slots:
    void testFrecencyRoundTrip();
    void testMapEncoding();
    void testSkipsUnknownFields();
    void testNegativeInt32();
};

void TestProto::testFrecencyRoundTrip()
{
    FrecencyItem thumbsup;
    thumbsup.totalUses = 7;
    thumbsup.recentUses = { 1700000000000ULL, 1700000060000ULL, 1700000120000ULL };
    thumbsup.frecency = -1;
    thumbsup.score = 270;

    FrecencyItem custom;
    custom.totalUses = 1;
    custom.recentUses = { 1699999999999ULL };
    custom.frecency = 42;
    custom.score = 100;

    FrecencyUserSettings settings;
    EmojiFrecency chat;
    chat.emojis.insert("thumbsup", thumbsup);
    chat.emojis.insert("123456789012345678", custom);
    settings.emojiFrecency = chat;

    EmojiFrecency reactions;
    reactions.emojis.insert("eyes", thumbsup);
    settings.emojiReactionFrecency = reactions;

    settings.dataVersion = 9;

    QByteArray encoded = settings.toProtoPartial();
    ProtoReader reader(encoded);
    FrecencyUserSettings decoded = FrecencyUserSettings::fromProto(reader);

    // toProtoPartial only carries the two frecency maps
    QVERIFY(!decoded.dataVersion.has_value());

    QVERIFY(decoded.emojiFrecency.has_value());
    QCOMPARE(decoded.emojiFrecency->emojis.size(), 2);

    const FrecencyItem &decodedThumbsup = decoded.emojiFrecency->emojis.value("thumbsup");
    QCOMPARE(decodedThumbsup.totalUses, 7u);
    QCOMPARE(decodedThumbsup.recentUses, thumbsup.recentUses);
    QCOMPARE(decodedThumbsup.frecency, -1);
    QCOMPARE(decodedThumbsup.score, 270);

    const FrecencyItem &decodedCustom = decoded.emojiFrecency->emojis.value("123456789012345678");
    QCOMPARE(decodedCustom.totalUses, 1u);
    QCOMPARE(decodedCustom.recentUses, custom.recentUses);
    QCOMPARE(decodedCustom.frecency, 42);
    QCOMPARE(decodedCustom.score, 100);

    QVERIFY(decoded.emojiReactionFrecency.has_value());
    QCOMPARE(decoded.emojiReactionFrecency->emojis.size(), 1);
    QCOMPARE(decoded.emojiReactionFrecency->emojis.value("eyes").totalUses, 7u);
}

void TestProto::testMapEncoding()
{
    FrecencyItem item;
    item.totalUses = 1;
    item.recentUses = { 2 };

    FrecencyUserSettings settings;
    EmojiFrecency chat;
    chat.emojis.insert("a", item);
    settings.emojiFrecency = chat;

    // 32 0c            field 6 (emoji_frecency), 12 bytes
    //   0a 0a          field 1 (map entry), 10 bytes
    //     0a 01 61     key = "a"
    //     12 05        value, 5 bytes
    //       08 01      total_uses = 1
    //       12 01 02   recent_uses = [2], packed
    QCOMPARE(settings.toProtoPartial().toHex(), QByteArray("320c0a0a0a016112050801120102"));
}

void TestProto::testSkipsUnknownFields()
{
    // 0a 06 08 01 10 02 18 2a          versions { client=1, server=2, data=42 }
    // 38 05                            unknown varint field 7
    // 2a 09 0a 03 61 62 63 0a 02 64 65 favorite_emojis { "abc", "de" } (not decoded)
    // 42 02 ff ff                      unknown length-delimited field 8
    QByteArray fixture = QByteArray::fromHex("0a0608011002182a"
                                             "3805"
                                             "2a090a036162630a026465"
                                             "4202ffff");

    ProtoReader reader(fixture);
    FrecencyUserSettings settings = FrecencyUserSettings::fromProto(reader);

    QVERIFY(settings.dataVersion.has_value());
    QCOMPARE(*settings.dataVersion, 42u);

    QVERIFY(!settings.emojiFrecency.has_value());
    QVERIFY(!settings.emojiReactionFrecency.has_value());
}

void TestProto::testNegativeInt32()
{
    ProtoWriter writer;
    writer.writeInt32(3, -1);

    // tag 0x18 then the sign-extended 10 byte varint protobuf uses for int32
    QCOMPARE(writer.bytes().toHex(), QByteArray("18ffffffffffffffffff01"));

    ProtoReader reader(writer.bytes());
    FrecencyItem item = FrecencyItem::fromProto(reader);
    QCOMPARE(item.frecency, -1);
}

QTEST_GUILESS_MAIN(TestProto)
#include "tst_Proto.moc"
