#include "Logging.hpp"

#include <iostream>

Q_LOGGING_CATEGORY(LogCore, "acheron.core");
Q_LOGGING_CATEGORY(LogNetwork, "acheron.network");
Q_LOGGING_CATEGORY(LogDiscord, "acheron.discord");
Q_LOGGING_CATEGORY(LogDB, "acheron.db");
Q_LOGGING_CATEGORY(LogUI, "acheron.ui");
Q_LOGGING_CATEGORY(LogProto, "acheron.proto");
Q_LOGGING_CATEGORY(LogDave, "acheron.dave");
Q_LOGGING_CATEGORY(LogVoice, "acheron.voice");

namespace Acheron {
namespace Core {

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

    stopping = false;
    writerThread = new std::thread(&Logger::writerLoop);

    qInstallMessageHandler(Logger::messageHandler);
}

void Logger::cleanup()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueCv.notify_one();

    if (writerThread) {
        if (writerThread->joinable())
            writerThread->join();
        delete writerThread;
        writerThread = nullptr;
    }

    if (logFile) {
        logFile->close();
        delete logFile;
        logFile = nullptr;
    }
}

void Logger::writerLoop()
{
    std::vector<std::string> batch;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [] { return !queue.empty() || stopping; });
            batch.swap(queue);
        }

        if (logFile && logFile->isOpen()) {
            for (const auto &line : batch) {
                logFile->write(line.data(), line.size());
                logFile->write("\n", 1);
            }
            logFile->flush();
        }
        batch.clear();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopping && queue.empty())
                break;
        }
    }
}

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const char *level;
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

    if (type == QtFatalMsg) {
        if (logFile && logFile->isOpen()) {
            auto utf8 = formatted.toUtf8();
            logFile->write(utf8);
            logFile->write("\n", 1);
            logFile->flush();
        }
        abort();
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back(formatted.toUtf8().toStdString());
    }
    queueCv.notify_one();
}

} // namespace Core
} // namespace Acheron
