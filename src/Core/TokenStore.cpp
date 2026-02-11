#include "TokenStore.hpp"

#include <QEventLoop>

#if __has_include(<qtkeychain/keychain.h>)
#  include <qtkeychain/keychain.h>
#else
#  include <qt6keychain/keychain.h>
#endif

#include "Logging.hpp"

namespace Acheron {
namespace Core {

QString TokenStore::keyForAccount(Snowflake accountId)
{
    return QString("account_%1_token").arg(accountId.toString());
}

bool TokenStore::saveToken(Snowflake accountId, const QString &token)
{
    QKeychain::WritePasswordJob job(SERVICE_NAME);
    job.setAutoDelete(false);
    job.setKey(keyForAccount(accountId));
    job.setTextData(token);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        qCWarning(LogCore) << "TokenStore: Failed to save token for account"
                           << accountId << ":" << job.errorString();
        return false;
    }

    return true;
}

QString TokenStore::loadToken(Snowflake accountId)
{
    QKeychain::ReadPasswordJob job(SERVICE_NAME);
    job.setAutoDelete(false);
    job.setKey(keyForAccount(accountId));

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        if (job.error() != QKeychain::EntryNotFound)
            qCWarning(LogCore) << "TokenStore: Failed to load token for account"
                               << accountId << ":" << job.errorString();
        return {};
    }

    return job.textData();
}

bool TokenStore::deleteToken(Snowflake accountId)
{
    QKeychain::DeletePasswordJob job(SERVICE_NAME);
    job.setAutoDelete(false);
    job.setKey(keyForAccount(accountId));

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        qCWarning(LogCore) << "TokenStore: Failed to delete token for account"
                           << accountId << ":" << job.errorString();
        return false;
    }

    return true;
}

} // namespace Core
} // namespace Acheron
