#pragma once

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(LogCore);
Q_DECLARE_LOGGING_CATEGORY(LogNetwork);
Q_DECLARE_LOGGING_CATEGORY(LogDiscord);
Q_DECLARE_LOGGING_CATEGORY(LogDB);
Q_DECLARE_LOGGING_CATEGORY(LogUI);

namespace Acheron {
namespace Core {

class Logger
{
public:
    static void init();
    static void cleanup();

private:
    static void messageHandler(QtMsgType type, const QMessageLogContext &context,
                               const QString &msg);

    static QFile *logFile;
    static QMutex fileMutex;
};

} // namespace Core
} // namespace Acheron
