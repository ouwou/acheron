#include "Discord/Entities.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using Acheron::Discord::Message;
using Acheron::Discord::MessageFlag;

class TestMessageEntities : public QObject
{
    Q_OBJECT

private slots:
    void forwardedSnapshotProvidesVisiblePayload();
};

void TestMessageEntities::forwardedSnapshotProvidesVisiblePayload()
{
    QJsonObject attachment{
        { "id", "200" },
        { "filename", "forwarded.png" },
        { "size", 123 },
        { "url", "https://cdn.discordapp.com/attachments/forwarded.png" },
        { "proxy_url", "https://media.discordapp.net/attachments/forwarded.png" },
        { "content_type", "image/png" },
        { "width", 16 },
        { "height", 16 },
    };
    QJsonObject snapshotMessage{
        { "type", 0 },
        { "content", "The forwarded text" },
        { "embeds", QJsonArray{ QJsonObject{ { "type", "rich" },
                                                     { "description", "Forwarded embed" } } } },
        { "attachments", QJsonArray{ attachment } },
        { "flags", 0 },
        { "mentions", QJsonArray{} },
        { "mention_roles", QJsonArray{} },
    };
    QJsonObject json{
        { "id", "100" },
        { "channel_id", "10" },
        { "author", QJsonObject{ { "id", "20" }, { "username", "sender" },
                                 { "global_name", QJsonValue::Null },
                                 { "avatar", QJsonValue::Null } } },
        { "content", "" },
        { "timestamp", "2026-08-31T00:00:00.000Z" },
        { "type", 0 },
        { "flags", static_cast<int>(MessageFlag::HAS_SNAPSHOT) },
        { "attachments", QJsonArray{} },
        { "embeds", QJsonArray{} },
        { "mentions", QJsonArray{} },
        { "mention_roles", QJsonArray{} },
        { "message_reference", QJsonObject{ { "type", 1 }, { "message_id", "99" },
                                              { "channel_id", "9" } } },
        { "message_snapshots", QJsonArray{ QJsonObject{ { "message", snapshotMessage } } } },
    };

    const Message message = Message::fromJson(json);

    QVERIFY(message.isForwarded());
    QCOMPARE(message.content.get(), QStringLiteral("The forwarded text"));
    QVERIFY(message.attachments.hasValue());
    QCOMPARE(message.attachments->size(), 1);
    QCOMPARE(message.attachments->first().filename.get(), QStringLiteral("forwarded.png"));
    QVERIFY(message.embeds.hasValue());
    QCOMPARE(message.embeds->size(), 1);
    QCOMPARE(message.embeds->first().description.get(), QStringLiteral("Forwarded embed"));
    QVERIFY(message.embedsJson.contains(QStringLiteral("Forwarded embed")));
}

QTEST_GUILESS_MAIN(TestMessageEntities)
#include "tst_MessageEntities.moc"
