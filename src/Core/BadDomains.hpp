#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <array>
#include <vector>

namespace Acheron {
namespace Core {

// Discord's list of known harmful domains (SHA-256 hashes of the domain, as the official client
// uses them), fetched once at startup and kept as a sorted array of raw hashes (~32 bytes each).
class BadDomains : public QObject
{
    Q_OBJECT
public:
    using Hash = std::array<unsigned char, 32>;

    static BadDomains *instance();

    void fetch(); // once per run; later calls do nothing

    // the link's host or any domain above it (sub.bad.example -> bad.example) is listed
    [[nodiscard]] bool isHarmful(const QUrl &url) const;

    // parsing and matching, also for tests
    [[nodiscard]] static std::vector<Hash> parseHashes(const QByteArray &json);
    [[nodiscard]] static bool matches(const std::vector<Hash> &sortedHashes, const QString &host);

private:
    using QObject::QObject;

    std::vector<Hash> hashes;
    bool fetchStarted = false;
};

} // namespace Core
} // namespace Acheron
