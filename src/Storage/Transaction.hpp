#pragma once

#include <QSqlDatabase>
#include <QString>

namespace Acheron {
namespace Storage {

class Transaction
{
public:
    explicit Transaction(QSqlDatabase &db);
    ~Transaction();

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    bool commit();

    bool ownsTransaction() const { return owns; }

private:
    QSqlDatabase db;
    QString connName;
    bool owns = false;
    bool finished = false;
};

} // namespace Storage
} // namespace Acheron
