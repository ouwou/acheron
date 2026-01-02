#include "Logging.hpp"

#include <iostream>

Q_LOGGING_CATEGORY(LogCore, "acheron.core");
Q_LOGGING_CATEGORY(LogNetwork, "acheron.network");
Q_LOGGING_CATEGORY(LogDiscord, "acheron.discord");
Q_LOGGING_CATEGORY(LogDB, "acheron.db");
Q_LOGGING_CATEGORY(LogUI, "acheron.ui");

namespace Acheron {
namespace Core {

QFile *Logger::logFile = nullptr;
QMutex Logger::fileMutex;

void Logger::init()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(path);
    if (!dir.exists())
        dir.mkpath(".");

    QString filePath = dir.filePath("acheron.log");

    logFile = new QFile(filePath);
    if (logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        QTextStream stream(logFile);
        stream << "\n=== Acheron startup: " << QDateTime::currentDateTime().toString() << " ===\n";
    }

    qInstallMessageHandler(Logger::messageHandler);
}

void Logger::cleanup()
{
    if (logFile) {
        logFile->close();
        delete logFile;
        logFile = nullptr;
    }
}

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString level;
    switch (type) {
    case QtDebugMsg:
        level = "DBG";
        break;
    case QtInfoMsg:
        level = "INF";
        break;
    case QtWarningMsg:
        level = "WRN";
        break;
    case QtCriticalMsg:
        level = "CRT";
        break;
    case QtFatalMsg:
        level = "FTL";
        break;
    }

    QString time = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    QString threadId = QString::number(reinterpret_cast<quint64>(QThread::currentThreadId()), 16);

    QString formatted =
            QString("[%1] [T:%2] [%3] [%4] %5").arg(time, threadId, level, context.category, msg);

    std::cout << formatted.toStdString() << std::endl;

    if (logFile && logFile->isOpen()) {
        QMutexLocker lock(&fileMutex);
        QTextStream stream(logFile);
        stream << formatted << "\n";
    }

    if (type == QtFatalMsg)
        abort();
}

} // namespace Core
} // namespace Acheron
