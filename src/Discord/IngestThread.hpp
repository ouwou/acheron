#pragma once

#include <QObject>
#include <QtZlib/zlib.h>

#include <deque>

namespace Acheron {
namespace Discord {

class IngestThread : public QObject
{
    Q_OBJECT
public:
    explicit IngestThread(QObject *parent = nullptr);
    ~IngestThread() override;

    void start();
    void stop();

    void push(const QByteArray &data);

signals:
    void payloadReceived(QJsonObject root);

private:
    void threadLoop();

private:
    std::thread thread;
    std::atomic<bool> running = false;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QByteArray> queue;
    QByteArray decompressedBuffer;

    z_stream stream;
    bool streamActive = false;
};

} // namespace Discord
} // namespace Acheron
