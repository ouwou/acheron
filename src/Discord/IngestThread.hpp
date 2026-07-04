#pragma once

#include <QObject>
#include <QtZlib/zlib.h>

#include <deque>
#include <thread>

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
    void reset();

signals:
    void payloadReceived(QJsonObject root);

private:
    struct TaggedData
    {
        QByteArray data;
        uint64_t generation;
    };

    void threadLoop();

private:
    std::thread thread;
    std::atomic<bool> running = false;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<TaggedData> queue;
    QByteArray decompressedBuffer;

    z_stream stream;
    bool streamActive = false;
    std::atomic<uint64_t> generation{ 0 };
};

} // namespace Discord
} // namespace Acheron
