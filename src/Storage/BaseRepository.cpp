#include "BaseRepository.hpp"

namespace Acheron {
namespace Storage {
BaseRepository::BaseRepository(const QString &connName) : connName(connName) { }

QSqlDatabase BaseRepository::getDb() const
{
    return QSqlDatabase::database(connName);
}

} // namespace Storage
} // namespace Acheron
