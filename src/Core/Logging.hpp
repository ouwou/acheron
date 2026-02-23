#pragma once

#include <QLoggingCategory>

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
Q_DECLARE_LOGGING_CATEGORY(LogDave);
Q_DECLARE_LOGGING_CATEGORY(LogVoice);

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

    inline static QFile *logFile = nullptr;

    inline static std::thread *writerThread = nullptr;
    inline static std::mutex queueMutex;
    inline static std::condition_variable queueCv;
    inline static std::vector<std::string> queue;
    inline static bool stopping = false;
};

} // namespace Core
} // namespace Acheron
