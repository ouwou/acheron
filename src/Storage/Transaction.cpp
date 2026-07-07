#include "Transaction.hpp"

#include "Core/Logging.hpp"

#include <QSet>
#include <QSqlError>

namespace Acheron {
namespace Storage {

namespace {
thread_local QSet<QString> activeTransactions;
} // namespace

Transaction::Transaction(QSqlDatabase &db)
    : db(db),
      connName(db.connectionName())
{
    if (activeTransactions.contains(connName))
        return;

    if (!db.transaction()) {
        qCWarning(LogDB) << "Failed to begin transaction on" << connName << ":" << db.lastError().text();
        return;
    }

    activeTransactions.insert(connName);
    owns = true;
}

Transaction::~Transaction()
{
    if (!owns || finished)
        return;

    db.rollback();
    activeTransactions.remove(connName);
}

bool Transaction::commit()
{
    if (!owns || finished)
        return true;

    finished = true;
    activeTransactions.remove(connName);

    if (!db.commit()) {
        qCWarning(LogDB) << "Failed to commit transaction on" << connName << ":" << db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

} // namespace Storage
} // namespace Acheron
