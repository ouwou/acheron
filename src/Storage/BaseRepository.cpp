#include "BaseRepository.hpp"

#include "Core/Logging.hpp"

#include <QSqlError>

namespace Acheron {
namespace Storage {
BaseRepository::BaseRepository(const QString &connName) : connName(connName) {}

QSqlDatabase BaseRepository::getDb() const
{
    return QSqlDatabase::database(connName);
}

bool BaseRepository::execLogged(QSqlQuery &q, const char *context)
{
    if (q.exec())
        return true;

    qCWarning(LogDB) << context << "failed:" << q.lastError().text();
    return false;
}

} // namespace Storage
} // namespace Acheron
