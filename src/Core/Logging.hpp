#pragma once

#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(LogCore);
Q_DECLARE_LOGGING_CATEGORY(LogNetwork);
Q_DECLARE_LOGGING_CATEGORY(LogDiscord);
Q_DECLARE_LOGGING_CATEGORY(LogDB);
Q_DECLARE_LOGGING_CATEGORY(LogUI);
Q_DECLARE_LOGGING_CATEGORY(LogProto);
#ifndef ACHERON_NO_VOICE
Q_DECLARE_LOGGING_CATEGORY(LogDave);
#endif
Q_DECLARE_LOGGING_CATEGORY(LogMiniaudio);
Q_DECLARE_LOGGING_CATEGORY(LogVoice);
Q_DECLARE_LOGGING_CATEGORY(LogVideo);

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
    static void writerLoop();
    static void rotateLogFile();

    inline static QFile *logFile = nullptr;
    inline static QString logFilePath;

    inline static std::thread *writerThread = nullptr;
    inline static std::mutex queueMutex;
    inline static std::condition_variable queueCv;
    inline static std::vector<std::string> queue;
    inline static bool stopping = false;

    static constexpr qint64 maxLogFileSize = 10 * 1024 * 1024;
    static constexpr int maxBackupFiles = 5;
};

} // namespace Core
} // namespace Acheron
