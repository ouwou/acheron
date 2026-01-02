#pragma once

#include <QSqlDatabase>

namespace Acheron {
namespace Storage {

class BaseRepository
{
public:
    BaseRepository(const QString &connName);

protected:
    QSqlDatabase getDb() const;

    QString connName;
};

} // namespace Storage
} // namespace Acheron
