#include "CurlUtils.hpp"

#include "Core/Logging.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

namespace Acheron {
namespace Discord {
namespace CurlUtils {

static int cachedBuildNumber = 0;
static constexpr int fallbackBuildNumber = 482285;

void fetchBuildNumber(QNetworkAccessManager *nam)
{
    qCInfo(LogNetwork) << "Fetching Discord build number...";

    QNetworkRequest request(QUrl("https://discord.com/app"));
    request.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());

    QNetworkReply *appReply = nam->get(request);
    QObject::connect(appReply, &QNetworkReply::finished, [nam, appReply]() {
        appReply->deleteLater();

        if (appReply->error() != QNetworkReply::NoError) {
            qCWarning(LogNetwork) << "Failed to fetch Discord app page:" << appReply->errorString();
            return;
        }

        QByteArray appPage = appReply->readAll();
        QRegularExpression sentryRegex("/assets/sentry.*?\\.js");
        QRegularExpressionMatch sentryMatch = sentryRegex.match(appPage);

        if (!sentryMatch.hasMatch()) {
            qCWarning(LogNetwork) << "Failed to find sentry JS path";
            return;
        }

        QString sentryPath = sentryMatch.captured(0);
        QString sentryUrl = "https://discord.com" + sentryPath;

        QNetworkRequest sentryRequest(sentryUrl);
        sentryRequest.setHeader(QNetworkRequest::UserAgentHeader, getUserAgent());

        QNetworkReply *sentryReply = nam->get(sentryRequest);
        QObject::connect(sentryReply, &QNetworkReply::finished, [sentryReply]() {
            sentryReply->deleteLater();

            if (sentryReply->error() != QNetworkReply::NoError) {
                qCWarning(LogNetwork) << "Failed to fetch sentry JS:" << sentryReply->errorString();
                return;
            }

            QByteArray sentryJs = sentryReply->readAll();
            QRegularExpression buildRegex("buildNumber\",\"(\\d+)");
            QRegularExpressionMatch buildMatch = buildRegex.match(sentryJs);

            if (!buildMatch.hasMatch()) {
                qCWarning(LogNetwork) << "Failed to extract build number";
                return;
            }

            cachedBuildNumber = buildMatch.captured(1).toInt();
            qCInfo(LogNetwork) << "Discord build number:" << cachedBuildNumber;
        });
    });
}

int getBuildNumber()
{
    if (cachedBuildNumber > 0)
        return cachedBuildNumber;
    return fallbackBuildNumber;
}

QString getCertificatePath()
{
    QString execDir = QCoreApplication::applicationDirPath();
    QString certPath = QDir(execDir).filePath("certs/cacert.pem");
    if (QFile::exists(certPath))
        return certPath;

    qCWarning(LogNetwork) << "Certificate file not found at" << certPath;
    return QString();
}

QString getUserAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/142.0.0.0 Safari/537.36";
}

QString getImpersonateTarget()
{
    return "chrome142";
}

UserAgentProps getUserAgentProps()
{
    return { "Windows", "Chrome", "142.0.0.0", "10" };
}

} // namespace CurlUtils
} // namespace Discord
} // namespace Acheron
