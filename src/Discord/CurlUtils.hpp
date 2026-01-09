#pragma once

#include <QString>
#include <QObject>

class QNetworkAccessManager;

namespace Acheron {
namespace Discord {
namespace CurlUtils {

struct UserAgentProps
{
    QString os;
    QString browser;
    QString browserVersion;
    QString osVersion;
};

QString getCertificatePath();
QString getUserAgent();
QString getImpersonateTarget();
UserAgentProps getUserAgentProps();

void fetchBuildNumber(QNetworkAccessManager *nam);
int getBuildNumber();

} // namespace CurlUtils
} // namespace Discord
} // namespace Acheron
