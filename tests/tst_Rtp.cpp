// the cipher cases include known-answer vectors built here with libsodium's detached-nonce primitives,
// so a mistake in which bytes are authenticated, or where the tag and nonce go, fails even though seal
// and open would still round-trip

#include "Discord/Voice/PacketBuffer.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/VoiceEncryption.hpp"

#include <QTest>

#include <sodium.h>

#include <cstring>

using namespace Acheron::Discord::Voice;

Q_DECLARE_METATYPE(EncryptionMode)
Q_DECLARE_METATYPE(PacketKind)

namespace {

std::span<const uint8_t> bytesOf(const QByteArray &bytes)
{
    return { reinterpret_cast<const uint8_t *>(bytes.constData()), static_cast<size_t>(bytes.size()) };
}

QByteArray toByteArray(std::span<const uint8_t> bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()));
}

void fill(PacketBuffer &packet, const QByteArray &content)
{
    packet.clear();
    QVERIFY(packet.append(bytesOf(content)));
}

QByteArray testKey()
{
    QByteArray key(32, '\0');
    for (int i = 0; i < key.size(); ++i)
        key[i] = char(i * 7 + 1);
    return key;
}

QByteArray referenceSeal(EncryptionMode mode, const QByteArray &key, const QByteArray &clear, const QByteArray &plaintext, quint32 counter)
{
    unsigned char nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
    nonce[0] = uchar(counter >> 24);
    nonce[1] = uchar(counter >> 16);
    nonce[2] = uchar(counter >> 8);
    nonce[3] = uchar(counter);

    QByteArray sealed(plaintext.size() + 16, '\0');
    unsigned long long sealedSize = 0;
    const auto *m = reinterpret_cast<const unsigned char *>(plaintext.constData());
    const auto *ad = reinterpret_cast<const unsigned char *>(clear.constData());
    const auto *k = reinterpret_cast<const unsigned char *>(key.constData());
    auto *c = reinterpret_cast<unsigned char *>(sealed.data());

    if (mode == EncryptionMode::AEAD_AES256_GCM_RTPSIZE)
        crypto_aead_aes256gcm_encrypt(c, &sealedSize, m, plaintext.size(), ad, clear.size(), nullptr, nonce, k);
    else
        crypto_aead_xchacha20poly1305_ietf_encrypt(c, &sealedSize, m, plaintext.size(), ad, clear.size(), nullptr, nonce, k);

    return clear + sealed.left(int(sealedSize)) + QByteArray(reinterpret_cast<const char *>(nonce), 4);
}

void addCipherModes()
{
    QTest::addColumn<EncryptionMode>("mode");
    QTest::newRow("xchacha20") << EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE;
    if (VoiceEncryption::isModeAvailable(EncryptionMode::AEAD_AES256_GCM_RTPSIZE))
        QTest::newRow("aes256gcm") << EncryptionMode::AEAD_AES256_GCM_RTPSIZE;
}

} // namespace

class TestRtp : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void classifiesBySecondByte_data();
    void classifiesBySecondByte();
    void classifyRejectsNonRtp_data();
    void classifyRejectsNonRtp();

    void parsesFixedHeader();
    void clearSizeCoversCsrcsAndExtensionPreamble_data();
    void clearSizeCoversCsrcsAndExtensionPreamble();
    void parseRejectsTruncatedHeaders_data();
    void parseRejectsTruncatedHeaders();
    void paddingIsCountedByTheLastByte();
    void parseSurvivesTruncationFuzz();

    void writesFixedHeader();
    void writesExtensionPreamble();
    void writeRefusesWhatItCannotExpress();

    void findsExtensionElements();
    void extensionLookupSkipsPaddingAndStopsAtReserved();
    void extensionLookupSurvivesTruncation();
    void writesExtensionsPaddedToWords();
    void writeExtensionsRefusesOneByteFormViolations();
    void writtenExtensionsAreFound();

    void sealMatchesReferenceLayout_data();
    void sealMatchesReferenceLayout();
    void sealAdvancesNonce_data();
    void sealAdvancesNonce();
    void opensReferencePacket_data();
    void opensReferencePacket();
    void rtcpKeepsEightBytesClear_data();
    void rtcpKeepsEightBytesClear();
    void openRejectsTampering_data();
    void openRejectsTampering();
    void openRejectsShortPackets_data();
    void openRejectsShortPackets();
    void sealNeedsTailroom();

    void bufferKeepsHeadroomForRelayHeader();
    void bufferBoundsAreEnforced();
    void poolRecyclesBuffers();
    void poolHandleOutlivesPool();
    void poolCapsIdleBuffers();
};

void TestRtp::initTestCase()
{
    QVERIFY(VoiceEncryption::initialize());
}

void TestRtp::classifiesBySecondByte_data()
{
    QTest::addColumn<int>("secondByte");
    QTest::addColumn<PacketKind>("kind");

    QTest::newRow("opus") << 120 << PacketKind::Rtp;
    QTest::newRow("opus with marker") << (0x80 | 120) << PacketKind::Rtp;
    QTest::newRow("video 101") << 101 << PacketKind::Rtp;
    QTest::newRow("video 101 with marker") << (0x80 | 101) << PacketKind::Rtp;
    QTest::newRow("rtx 108 with marker") << (0x80 | 108) << PacketKind::Rtp;
    QTest::newRow("just below the rtcp range") << 193 << PacketKind::Rtp;
    QTest::newRow("first rtcp value") << 194 << PacketKind::Rtcp;
    QTest::newRow("sender report") << 200 << PacketKind::Rtcp;
    QTest::newRow("receiver report") << 201 << PacketKind::Rtcp;
    QTest::newRow("transport feedback") << 205 << PacketKind::Rtcp;
    QTest::newRow("payload feedback") << 206 << PacketKind::Rtcp;
    QTest::newRow("last rtcp value") << 223 << PacketKind::Rtcp;
    QTest::newRow("just above the rtcp range") << 224 << PacketKind::Rtp;
}

void TestRtp::classifiesBySecondByte()
{
    QFETCH(int, secondByte);
    QFETCH(PacketKind, kind);

    QByteArray packet(32, '\0');
    packet[0] = char(0x80);
    packet[1] = char(secondByte);

    QCOMPARE(classifyPacket(bytesOf(packet)), kind);
}

void TestRtp::classifyRejectsNonRtp_data()
{
    QTest::addColumn<QByteArray>("packet");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("shorter than an rtcp header") << QByteArray("\x80\xc8\x00\x01\x00\x00\x00", 7);
    QTest::newRow("version 0") << QByteArray(32, '\0');
    QTest::newRow("version 1") << (QByteArray(1, '\x40') + QByteArray(31, '\0'));
    QTest::newRow("version 3") << (QByteArray(1, '\xc0') + QByteArray(31, '\0'));
    QTest::newRow("rtp shorter than its fixed header") << QByteArray("\x80\x78\x00\x01\x00\x00\x00\x00\x00\x00\x00", 11);
    // IP discovery responses share the socket and start with a 16-bit type of 2
    QTest::newRow("ip discovery response") << (QByteArray("\x00\x02\x00\x46", 4) + QByteArray(70, '\0'));
}

void TestRtp::classifyRejectsNonRtp()
{
    QFETCH(QByteArray, packet);
    QCOMPARE(classifyPacket(bytesOf(packet)), PacketKind::Unknown);
}

void TestRtp::parsesFixedHeader()
{
    const QByteArray packet = QByteArray::fromHex("80f8123456789abcdeadbeef") + QByteArray("payload");

    const auto parsed = parseRtp(bytesOf(packet));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->header.padding, false);
    QCOMPARE(parsed->header.extension, false);
    QCOMPARE(int(parsed->header.csrcCount), 0);
    QCOMPARE(parsed->header.marker, true);
    QCOMPARE(int(parsed->header.payloadType), 120);
    QCOMPARE(parsed->header.sequence, quint16(0x1234));
    QCOMPARE(parsed->header.timestamp, quint32(0x56789abc));
    QCOMPARE(parsed->header.ssrc, quint32(0xdeadbeef));
    QCOMPARE(parsed->clearSize, 12);
    QCOMPARE(parsed->extensionSize, 0);
}

void TestRtp::clearSizeCoversCsrcsAndExtensionPreamble_data()
{
    QTest::addColumn<QByteArray>("packet");
    QTest::addColumn<int>("clearSize");
    QTest::addColumn<int>("extensionSize");

    const QByteArray rest = QByteArray::fromHex("78000100000000000000aa");
    const QByteArray csrc = QByteArray::fromHex("01020304");
    const QByteArray payload(40, 'p');

    QTest::newRow("plain") << (QByteArray(1, '\x80') + rest + payload) << 12 << 0;
    QTest::newRow("two csrcs") << (QByteArray(1, '\x82') + rest + csrc + csrc + payload) << 20 << 0;
    QTest::newRow("extension, one word") << (QByteArray(1, '\x90') + rest + QByteArray::fromHex("bede0001") + payload) << 16 << 4;
    QTest::newRow("extension, three words") << (QByteArray(1, '\x90') + rest + QByteArray::fromHex("bede0003") + payload) << 16 << 12;
    QTest::newRow("extension, zero words") << (QByteArray(1, '\x90') + rest + QByteArray::fromHex("bede0000") + payload) << 16 << 0;
    QTest::newRow("csrc and extension") << (QByteArray(1, '\x91') + rest + csrc + QByteArray::fromHex("bede0002") + payload) << 20 << 8;
}

void TestRtp::clearSizeCoversCsrcsAndExtensionPreamble()
{
    QFETCH(QByteArray, packet);
    QFETCH(int, clearSize);
    QFETCH(int, extensionSize);

    const auto parsed = parseRtp(bytesOf(packet));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->clearSize, clearSize);
    QCOMPARE(parsed->extensionSize, extensionSize);
}

void TestRtp::parseRejectsTruncatedHeaders_data()
{
    QTest::addColumn<QByteArray>("packet");

    const QByteArray rest = QByteArray::fromHex("78000100000000000000aa");

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("eleven bytes") << (QByteArray(1, '\x80') + rest).left(11);
    QTest::newRow("wrong version") << (QByteArray(1, '\x40') + rest + QByteArray(20, 'p'));
    QTest::newRow("csrc list cut short") << (QByteArray(1, '\x83') + rest + QByteArray(8, 'c'));
    QTest::newRow("extension preamble missing") << (QByteArray(1, '\x90') + rest);
    QTest::newRow("extension preamble cut short") << (QByteArray(1, '\x90') + rest + QByteArray::fromHex("bede00"));
}

void TestRtp::paddingIsCountedByTheLastByte()
{
    // Discord fills the padding with the count; the standard only promises the last byte
    const std::vector<uint8_t> padded = { 0xAA, 0xBB, 0x03, 0x03, 0x03 };
    QCOMPARE(rtpPaddingSize(padded), std::optional<int>(3));
    const std::vector<uint8_t> zeroFilled = { 0xAA, 0x00, 0x00, 0x00, 0x04 };
    QCOMPARE(rtpPaddingSize(zeroFilled), std::optional<int>(4));
    const std::vector<uint8_t> allPadding = { 0x02, 0x02 };
    QCOMPARE(rtpPaddingSize(allPadding), std::optional<int>(2));

    const std::vector<uint8_t> zero = { 0xAA, 0x00 };
    QVERIFY(!rtpPaddingSize(zero));
    const std::vector<uint8_t> tooLong = { 0xAA, 0x09 };
    QVERIFY(!rtpPaddingSize(tooLong));
    QVERIFY(!rtpPaddingSize({}));
}

void TestRtp::parseRejectsTruncatedHeaders()
{
    QFETCH(QByteArray, packet);
    QVERIFY(!parseRtp(bytesOf(packet)).has_value());
}

void TestRtp::parseSurvivesTruncationFuzz()
{
    QByteArray packet = QByteArray::fromHex("9f78000100000000000000aa") + QByteArray(15 * 4, 'c') + QByteArray::fromHex("bede0004") + QByteArray(64, 'p');

    for (int length = 0; length <= packet.size(); ++length) {
        const auto parsed = parseRtp(bytesOf(packet.left(length)));
        if (parsed.has_value())
            QVERIFY2(parsed->clearSize <= length, qPrintable(QStringLiteral("prefix %1 reported clear size %2").arg(length).arg(parsed->clearSize)));
    }
}

void TestRtp::writesFixedHeader()
{
    RtpHeader header;
    header.marker = true;
    header.payloadType = 120;
    header.sequence = 0x1234;
    header.timestamp = 0x56789abc;
    header.ssrc = 0xdeadbeef;

    uint8_t out[16] = {};
    QCOMPARE(writeRtpHeader(header, out), 12);
    QCOMPARE(toByteArray({ out, 12 }), QByteArray::fromHex("80f8123456789abcdeadbeef"));
}

void TestRtp::writesExtensionPreamble()
{
    RtpHeader header;
    header.extension = true;
    header.payloadType = 101;
    header.sequence = 1;
    header.ssrc = 0xaa;

    uint8_t out[16] = {};
    QCOMPARE(writeRtpHeader(header, out, 3), 16);
    QCOMPARE(toByteArray(out), QByteArray::fromHex("90650001") + QByteArray::fromHex("00000000000000aa") + QByteArray::fromHex("bede0003"));
}

void TestRtp::writeRefusesWhatItCannotExpress()
{
    RtpHeader header;
    uint8_t out[16] = {};

    QCOMPARE(writeRtpHeader(header, { out, 11 }), -1);

    header.extension = true;
    QCOMPARE(writeRtpHeader(header, { out, 15 }, 1), -1);
    QCOMPARE(writeRtpHeader(header, out, 0x10000), -1);
    QCOMPARE(writeRtpHeader(header, out, -1), -1);

    header.extension = false;
    header.csrcCount = 1;
    QCOMPARE(writeRtpHeader(header, out), -1);
}

void TestRtp::findsExtensionElements()
{
    // abs-send-time (id 3, 3 bytes), transport-wide-cc (id 5, 2 bytes), audio level (id 1, 1 byte)
    const QByteArray elements = QByteArray::fromHex("32aabbcc" "511234" "107f" "000000");

    QCOMPARE(toByteArray(findRtpExtension(bytesOf(elements), 3)), QByteArray::fromHex("aabbcc"));
    QCOMPARE(toByteArray(findRtpExtension(bytesOf(elements), 5)), QByteArray::fromHex("1234"));
    QCOMPARE(toByteArray(findRtpExtension(bytesOf(elements), 1)), QByteArray::fromHex("7f"));
    QVERIFY(findRtpExtension(bytesOf(elements), 2).empty());
    QVERIFY(findRtpExtension({}, 3).empty());
}

void TestRtp::extensionLookupSkipsPaddingAndStopsAtReserved()
{
    const QByteArray padded = QByteArray::fromHex("0000" "107f" "00" "5112340000");
    QCOMPARE(toByteArray(findRtpExtension(bytesOf(padded), 5)), QByteArray::fromHex("1234"));

    // id 15 ends the block; whatever follows it must not be read as elements
    const QByteArray stopped = QByteArray::fromHex("107f" "f0" "511234");
    QCOMPARE(toByteArray(findRtpExtension(bytesOf(stopped), 1)), QByteArray::fromHex("7f"));
    QVERIFY(findRtpExtension(bytesOf(stopped), 5).empty());
}

void TestRtp::extensionLookupSurvivesTruncation()
{
    const QByteArray elements = QByteArray::fromHex("32aabbcc" "5f000102030405060708090a0b0c0d0e0f");

    for (int length = 0; length <= elements.size(); ++length) {
        const auto found = findRtpExtension(bytesOf(elements.left(length)), 5);
        if (length < elements.size())
            QVERIFY2(found.empty(), qPrintable(QStringLiteral("prefix %1 returned a value that overruns it").arg(length)));
        else
            QCOMPARE(int(found.size()), 16);
    }
}

void TestRtp::writesExtensionsPaddedToWords()
{
    const QByteArray sendTime = QByteArray::fromHex("aabbcc");
    const QByteArray sequence = QByteArray::fromHex("1234");
    const RtpExtension elements[] = { { 3, bytesOf(sendTime) }, { 5, bytesOf(sequence) } };

    uint8_t out[16];
    std::memset(out, 0xee, sizeof(out));
    QCOMPARE(writeRtpExtensions(elements, out), 8);
    QCOMPARE(toByteArray({ out, 8 }), QByteArray::fromHex("32aabbcc" "511234" "00"));

    QCOMPARE(writeRtpExtensions({}, out), 0);
}

void TestRtp::writeExtensionsRefusesOneByteFormViolations()
{
    const QByteArray oneByte(1, 'v');
    const QByteArray threeBytes(3, 'v');
    const QByteArray seventeenBytes(17, 'v');
    uint8_t out[64] = {};

    const RtpExtension reservedId[] = { { 15, bytesOf(oneByte) } };
    QCOMPARE(writeRtpExtensions(reservedId, out), -1);

    const RtpExtension zeroId[] = { { 0, bytesOf(oneByte) } };
    QCOMPARE(writeRtpExtensions(zeroId, out), -1);

    const RtpExtension tooLong[] = { { 3, bytesOf(seventeenBytes) } };
    QCOMPARE(writeRtpExtensions(tooLong, out), -1);

    const RtpExtension empty[] = { { 3, {} } };
    QCOMPARE(writeRtpExtensions(empty, out), -1);

    const RtpExtension fits[] = { { 3, bytesOf(threeBytes) } };
    QCOMPARE(writeRtpExtensions(fits, { out, 3 }), -1);
    QCOMPARE(writeRtpExtensions(fits, { out, 4 }), 4);
}

void TestRtp::writtenExtensionsAreFound()
{
    const QByteArray longest(16, 'x');
    const QByteArray shortest(1, 'y');
    const RtpExtension elements[] = { { 14, bytesOf(longest) }, { 1, bytesOf(shortest) } };

    uint8_t out[32] = {};
    const int size = writeRtpExtensions(elements, out);
    QCOMPARE(size, 20);

    const std::span<const uint8_t> written(out, size_t(size));
    QCOMPARE(toByteArray(findRtpExtension(written, 14)), longest);
    QCOMPARE(toByteArray(findRtpExtension(written, 1)), shortest);
}

void TestRtp::sealMatchesReferenceLayout_data()
{
    addCipherModes();
}

void TestRtp::sealMatchesReferenceLayout()
{
    QFETCH(EncryptionMode, mode);

    const QByteArray key = testKey();
    const QByteArray clear = QByteArray::fromHex("9078000100000000000000aabede0001");
    const QByteArray plaintext = QByteArray::fromHex("10550000") + QByteArray("opus frame");

    VoiceEncryption cipher(mode, key);
    PacketBuffer packet;
    fill(packet, clear + plaintext);

    QVERIFY(cipher.seal(packet, clear.size()));
    QCOMPARE(packet.size(), clear.size() + plaintext.size() + VoiceEncryption::TRAILER_SIZE);
    QCOMPARE(toByteArray(packet.bytes()), referenceSeal(mode, key, clear, plaintext, 0));
}

void TestRtp::sealAdvancesNonce_data()
{
    addCipherModes();
}

void TestRtp::sealAdvancesNonce()
{
    QFETCH(EncryptionMode, mode);

    const QByteArray key = testKey();
    const QByteArray clear = QByteArray::fromHex("8078000100000000000000aa");
    const QByteArray plaintext("same payload every time");

    VoiceEncryption cipher(mode, key);
    for (quint32 counter = 0; counter < 3; ++counter) {
        PacketBuffer packet;
        fill(packet, clear + plaintext);
        QVERIFY(cipher.seal(packet, clear.size()));

        QByteArray appendedNonce(4, '\0');
        appendedNonce[3] = char(counter);

        const QByteArray sealed = toByteArray(packet.bytes());
        QCOMPARE(sealed.right(4), appendedNonce);
        QCOMPARE(sealed, referenceSeal(mode, key, clear, plaintext, counter));
    }
}

void TestRtp::opensReferencePacket_data()
{
    addCipherModes();
}

void TestRtp::opensReferencePacket()
{
    QFETCH(EncryptionMode, mode);

    const QByteArray key = testKey();
    const QByteArray clear = QByteArray::fromHex("9078000100000000000000aabede0001");
    const QByteArray plaintext = QByteArray::fromHex("10550000") + QByteArray("opus frame");

    const QByteArray sealed = referenceSeal(mode, key, clear, plaintext, 0x01020304);

    VoiceEncryption cipher(mode, key);
    PacketBuffer packet;
    fill(packet, sealed);

    QVERIFY(cipher.open(packet, clear.size()));
    QCOMPARE(toByteArray(packet.bytes()), clear + plaintext);
}

void TestRtp::rtcpKeepsEightBytesClear_data()
{
    addCipherModes();
}

void TestRtp::rtcpKeepsEightBytesClear()
{
    QFETCH(EncryptionMode, mode);

    const QByteArray key = testKey();
    const QByteArray rtcp = QByteArray::fromHex("81c90007" "000000aa") + QByteArray(24, 'r');

    VoiceEncryption cipher(mode, key);
    PacketBuffer packet;
    fill(packet, rtcp);

    QVERIFY(cipher.seal(packet, RTCP_CLEAR_SIZE));
    const QByteArray sealed = toByteArray(packet.bytes());
    QCOMPARE(sealed.left(RTCP_CLEAR_SIZE), rtcp.left(RTCP_CLEAR_SIZE));
    QVERIFY(sealed.mid(RTCP_CLEAR_SIZE, 24) != rtcp.mid(RTCP_CLEAR_SIZE));
    QCOMPARE(sealed, referenceSeal(mode, key, rtcp.left(RTCP_CLEAR_SIZE), rtcp.mid(RTCP_CLEAR_SIZE), 0));
    QCOMPARE(classifyPacket(packet.bytes()), PacketKind::Rtcp);

    QVERIFY(cipher.open(packet, RTCP_CLEAR_SIZE));
    QCOMPARE(toByteArray(packet.bytes()), rtcp);
}

void TestRtp::openRejectsTampering_data()
{
    QTest::addColumn<EncryptionMode>("mode");
    QTest::addColumn<int>("offsetFromEnd");
    QTest::addColumn<int>("offsetFromStart");

    const auto addRows = [](const char *name, EncryptionMode mode) {
        QTest::newRow(qPrintable(QStringLiteral("%1, clear header").arg(name))) << mode << -1 << 3;
        QTest::newRow(qPrintable(QStringLiteral("%1, extension preamble").arg(name))) << mode << -1 << 15;
        QTest::newRow(qPrintable(QStringLiteral("%1, ciphertext").arg(name))) << mode << -1 << 18;
        QTest::newRow(qPrintable(QStringLiteral("%1, tag").arg(name))) << mode << 6 << -1;
        QTest::newRow(qPrintable(QStringLiteral("%1, nonce").arg(name))) << mode << 1 << -1;
    };

    addRows("xchacha20", EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE);
    if (VoiceEncryption::isModeAvailable(EncryptionMode::AEAD_AES256_GCM_RTPSIZE))
        addRows("aes256gcm", EncryptionMode::AEAD_AES256_GCM_RTPSIZE);
}

void TestRtp::openRejectsTampering()
{
    QFETCH(EncryptionMode, mode);
    QFETCH(int, offsetFromEnd);
    QFETCH(int, offsetFromStart);

    const QByteArray key = testKey();
    const QByteArray clear = QByteArray::fromHex("9078000100000000000000aabede0001");
    QByteArray sealed = referenceSeal(mode, key, clear, QByteArray("payload under test"), 7);

    const int index = offsetFromStart >= 0 ? offsetFromStart : sealed.size() - offsetFromEnd;
    sealed[index] = char(sealed.at(index) ^ 0x01);

    VoiceEncryption cipher(mode, key);
    PacketBuffer packet;
    fill(packet, sealed);
    QVERIFY(!cipher.open(packet, clear.size()));
}

void TestRtp::openRejectsShortPackets_data()
{
    addCipherModes();
}

void TestRtp::openRejectsShortPackets()
{
    QFETCH(EncryptionMode, mode);

    const QByteArray key = testKey();
    const QByteArray clear = QByteArray::fromHex("8078000100000000000000aa");
    const QByteArray sealed = referenceSeal(mode, key, clear, QByteArray(), 0);
    QCOMPARE(sealed.size(), clear.size() + VoiceEncryption::TRAILER_SIZE);

    VoiceEncryption cipher(mode, key);
    for (int length = 0; length < sealed.size(); ++length) {
        PacketBuffer packet;
        fill(packet, sealed.left(length));
        QVERIFY2(!cipher.open(packet, qMin(length, int(clear.size()))), qPrintable(QStringLiteral("opened a %1-byte prefix").arg(length)));
    }

    PacketBuffer whole;
    fill(whole, sealed);
    QVERIFY(cipher.open(whole, clear.size()));
    QCOMPARE(whole.size(), int(clear.size()));
}

void TestRtp::sealNeedsTailroom()
{
    VoiceEncryption cipher(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE, testKey());

    PacketBuffer packet;
    packet.clear();
    QVERIFY(packet.resize(PacketBuffer::MAX_CONTENT - VoiceEncryption::TRAILER_SIZE + 1));
    std::memset(packet.data(), 0x80, size_t(packet.size()));
    const QByteArray before = toByteArray(packet.bytes());

    QVERIFY(!cipher.seal(packet, 12));
    QCOMPARE(toByteArray(packet.bytes()), before);

    QVERIFY(packet.resize(PacketBuffer::MAX_CONTENT - VoiceEncryption::TRAILER_SIZE));
    QVERIFY(cipher.seal(packet, 12));
    QCOMPARE(packet.size(), int(PacketBuffer::MAX_CONTENT));
}

void TestRtp::bufferKeepsHeadroomForRelayHeader()
{
    PacketBuffer packet;
    fill(packet, QByteArray("rtp packet"));

    // 22 bytes for an IPv6 destination
    QVERIFY(packet.headroom() >= 22);
    uint8_t *content = packet.data();

    QVERIFY(packet.growFront(22));
    QCOMPARE(packet.size(), 32);
    QCOMPARE(static_cast<const void *>(packet.data() + 22), static_cast<const void *>(content));

    QVERIFY(packet.dropFront(22));
    QCOMPARE(toByteArray(packet.bytes()), QByteArray("rtp packet"));
    QCOMPARE(static_cast<const void *>(packet.data()), static_cast<const void *>(content));
}

void TestRtp::bufferBoundsAreEnforced()
{
    PacketBuffer packet;
    packet.clear();

    QVERIFY(!packet.growFront(PacketBuffer::HEADROOM + 1));
    QVERIFY(!packet.dropFront(1));
    QVERIFY(!packet.resize(PacketBuffer::MAX_CONTENT + 1));
    QVERIFY(!packet.resize(-1));
    QCOMPARE(packet.size(), 0);

    QVERIFY(packet.resize(PacketBuffer::MAX_CONTENT));
    QCOMPARE(packet.tailroom(), 0);
    const uint8_t one = 1;
    QVERIFY(!packet.append({ &one, 1 }));

    QVERIFY(packet.dropFront(PacketBuffer::MAX_CONTENT));
    QCOMPARE(packet.size(), 0);
    QVERIFY(!packet.dropFront(1));
}

void TestRtp::poolRecyclesBuffers()
{
    PacketPool pool;
    QCOMPARE(pool.idleCount(), 0);

    PacketBuffer *first = nullptr;
    {
        PacketPool::Handle packet = pool.acquire();
        first = packet.get();
        QVERIFY(packet->append(bytesOf(QByteArray("left over from the last user"))));
        QVERIFY(packet->growFront(8));
    }
    QCOMPARE(pool.idleCount(), 1);

    PacketPool::Handle reused = pool.acquire();
    QCOMPARE(reused.get(), first);
    QCOMPARE(pool.idleCount(), 0);
    QCOMPARE(reused->size(), 0);
    QCOMPARE(reused->headroom(), int(PacketBuffer::HEADROOM));

    for (int i = 0; i < 1000; ++i) {
        PacketPool::Handle held = pool.acquire();
        PacketPool::Handle scratch = pool.acquire();
    }
    QCOMPARE(pool.idleCount(), 2);
}

void TestRtp::poolHandleOutlivesPool()
{
    PacketPool::Handle survivor;
    {
        PacketPool pool;
        survivor = pool.acquire();
        QVERIFY(survivor->append(bytesOf(QByteArray("still readable"))));
    }
    QCOMPARE(toByteArray(survivor->bytes()), QByteArray("still readable"));
    survivor.reset();
}

void TestRtp::poolCapsIdleBuffers()
{
    PacketPool pool;
    std::vector<PacketPool::Handle> burst;
    for (int i = 0; i < PacketPool::MAX_IDLE + 50; ++i)
        burst.push_back(pool.acquire());
    burst.clear();

    QCOMPARE(pool.idleCount(), int(PacketPool::MAX_IDLE));
}

QTEST_GUILESS_MAIN(TestRtp)
#include "tst_Rtp.moc"
