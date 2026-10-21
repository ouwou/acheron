// the store's rules are the official client's CallStore (docs/video-re/re-dm-calls.md)

#include "Core/Calls/CallStore.hpp"
#include "Discord/Events.hpp"
#include "Discord/Outbound.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace Acheron;
using Core::CallConnect;
using Core::CallStore;
using Core::Snowflake;

namespace {

const Snowflake CHANNEL(5000);

template <typename Event>
Event parseEvent(const char *json)
{
    return Event::fromJson(QJsonDocument::fromJson(json).object());
}

Discord::CallCreate ongoingCall()
{
    return parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{},"voice_states":[{"user_id":"111","channel_id":"5000","session_id":"abc","deaf":false,"mute":false,"self_deaf":false,"self_mute":true,"self_video":true,"self_stream":true,"suppress":false}]})");
}

} // namespace

class TestCalls : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void connectPayload();
    void createParsesCallAndItsVoiceStates();
    void ringsMapRungUserToRinger();
    void nullRegionIsNoRegion();

    void selectingADmAsksOnce();
    void aFreshSessionAsksAgain();
    void createMakesTheCallActive();
    void aCallWithoutRegionIsNotActive();
    void updateKeepsUnavailability();
    void updateForAnUnknownCallRecordsIt();
    void deleteLeavesAKnownButEndedCall();
    void unavailableKeepsTheCallButNotActive();
    void unavailableForAnUnknownCallIsNotActive();
    void ringingIsOnlyReportedForActiveCalls();
    void forgetAndResetDropCalls();
};

void TestCalls::initTestCase()
{
    qRegisterMetaType<Snowflake>();
}

void TestCalls::connectPayload()
{
    Discord::ConnectCall msg;
    msg.channelId = CHANNEL;
    QCOMPARE(msg.toJson().toBytes(), QByteArray(R"({"op":13,"d":{"channel_id":"5000"}})"));
}

void TestCalls::createParsesCallAndItsVoiceStates()
{
    const Discord::CallCreate call = ongoingCall();

    QCOMPARE(quint64(call.channelId.get()), quint64(5000));
    QCOMPARE(quint64(call.messageId.get()), quint64(6000));
    QCOMPARE(call.region.value_or(QString()), QStringLiteral("rotterdam"));
    QVERIFY(call.ongoingRings.isEmpty());

    QVERIFY(call.voiceStates.hasValue());
    QCOMPARE(call.voiceStates->size(), 1);
    const Discord::VoiceState &state = call.voiceStates->first();
    QCOMPARE(quint64(state.userId.get()), quint64(111));
    QCOMPARE(quint64(state.channelId.get()), quint64(5000));
    QCOMPARE(state.sessionId.get(), QStringLiteral("abc"));
    QVERIFY(!state.guildId.hasValue());
    QCOMPARE(state.selfMute.get(), true);
    QCOMPARE(state.selfVideo.get(), true);
    QCOMPARE(state.selfStream.valueOr(false), true);
}

void TestCalls::ringsMapRungUserToRinger()
{
    const auto update = parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111","333":null}})");

    QCOMPARE(update.ongoingRings.size(), 2);
    QCOMPARE(quint64(update.ongoingRings.value(Snowflake(222))), quint64(111));
    QVERIFY(update.ongoingRings.contains(Snowflake(333)));
    QVERIFY(!update.ongoingRings.value(Snowflake(333)).isValid());
}

void TestCalls::nullRegionIsNoRegion()
{
    QVERIFY(!parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":null,"ongoing_rings":{}})").region.has_value());
    QVERIFY(!parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","ongoing_rings":{}})").region.has_value());
}

void TestCalls::selectingADmAsksOnce()
{
    CallStore store;

    QVERIFY(store.claimConnect(CHANNEL, CallConnect::OncePerChannel));
    QVERIFY(!store.claimConnect(CHANNEL, CallConnect::OncePerChannel));
    QVERIFY(!store.isActive(CHANNEL));

    store.handleCallCreate(ongoingCall());
    QVERIFY(!store.claimConnect(CHANNEL, CallConnect::OncePerChannel));
}

void TestCalls::aFreshSessionAsksAgain()
{
    CallStore store;
    store.handleCallCreate(ongoingCall());

    QVERIFY(store.claimConnect(CHANNEL, CallConnect::Always));
    QVERIFY(store.claimConnect(CHANNEL, CallConnect::Always));

    store.reset();
    QVERIFY(store.claimConnect(CHANNEL, CallConnect::OncePerChannel));
}

void TestCalls::createMakesTheCallActive()
{
    CallStore store;
    QSignalSpy changed(&store, &CallStore::callChanged);

    store.handleCallCreate(ongoingCall());

    QVERIFY(store.isActive(CHANNEL));
    const auto call = store.call(CHANNEL);
    QVERIFY(call.has_value());
    QCOMPARE(quint64(call->messageId), quint64(6000));
    QCOMPARE(call->region.value_or(QString()), QStringLiteral("rotterdam"));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(quint64(changed.first().at(0).value<Snowflake>()), quint64(5000));
}

void TestCalls::aCallWithoutRegionIsNotActive()
{
    CallStore store;
    store.handleCallCreate(parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":null,"ongoing_rings":{"222":"111"}})"));

    QVERIFY(store.call(CHANNEL).has_value());
    QVERIFY(!store.isActive(CHANNEL));

    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111"}})"));
    QVERIFY(store.isActive(CHANNEL));
}

void TestCalls::updateKeepsUnavailability()
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"frankfurt","ongoing_rings":{}})"));

    QCOMPARE(store.call(CHANNEL)->region.value_or(QString()), QStringLiteral("frankfurt"));
    QVERIFY(store.call(CHANNEL)->unavailable);
    QVERIFY(!store.isActive(CHANNEL));

    store.handleCallCreate(ongoingCall());
    QVERIFY(store.isActive(CHANNEL));
}

void TestCalls::updateForAnUnknownCallRecordsIt()
{
    CallStore store;
    store.handleCallUpdate(parseEvent<Discord::CallUpdate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{}})"));

    QVERIFY(store.isActive(CHANNEL));
}

void TestCalls::deleteLeavesAKnownButEndedCall()
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    QSignalSpy changed(&store, &CallStore::callChanged);

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000"})"));

    QVERIFY(!store.isActive(CHANNEL));
    QCOMPARE(changed.size(), 1);

    const auto call = store.call(CHANNEL);
    QVERIFY(call.has_value());
    QVERIFY(!call->region.has_value());
    QVERIFY(!call->messageId.isValid());
    QVERIFY(call->ongoingRings.isEmpty());

    QVERIFY(!store.claimConnect(CHANNEL, CallConnect::OncePerChannel));
}

void TestCalls::unavailableKeepsTheCallButNotActive()
{
    CallStore store;
    store.handleCallCreate(ongoingCall());

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    const auto call = store.call(CHANNEL);
    QVERIFY(call.has_value());
    QVERIFY(call->unavailable);
    QCOMPARE(call->region.value_or(QString()), QStringLiteral("rotterdam"));
    QCOMPARE(quint64(call->messageId), quint64(6000));
    QVERIFY(!store.isActive(CHANNEL));
}

void TestCalls::unavailableForAnUnknownCallIsNotActive()
{
    CallStore store;
    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000","unavailable":true})"));

    QVERIFY(store.call(CHANNEL).has_value());
    QVERIFY(store.call(CHANNEL)->unavailable);
    QVERIFY(!store.isActive(CHANNEL));
}

void TestCalls::ringingIsOnlyReportedForActiveCalls()
{
    CallStore store;
    store.handleCallCreate(parseEvent<Discord::CallCreate>(R"({"channel_id":"5000","message_id":"6000","region":"rotterdam","ongoing_rings":{"222":"111"}})"));

    QVERIFY(store.isRinging(CHANNEL, Snowflake(222)));
    QVERIFY(!store.isRinging(CHANNEL, Snowflake(111)));
    QVERIFY(!store.isRinging(Snowflake(9999), Snowflake(222)));

    store.handleCallDelete(parseEvent<Discord::CallDelete>(R"({"channel_id":"5000"})"));
    QVERIFY(!store.isRinging(CHANNEL, Snowflake(222)));
}

void TestCalls::forgetAndResetDropCalls()
{
    CallStore store;
    store.handleCallCreate(ongoingCall());
    QSignalSpy changed(&store, &CallStore::callChanged);

    store.forget(CHANNEL);
    QVERIFY(!store.call(CHANNEL).has_value());
    QCOMPARE(changed.size(), 1);

    store.forget(CHANNEL);
    QCOMPARE(changed.size(), 1);

    store.handleCallCreate(ongoingCall());
    store.reset();
    QVERIFY(!store.call(CHANNEL).has_value());
}

QTEST_GUILESS_MAIN(TestCalls)
#include "tst_Calls.moc"
