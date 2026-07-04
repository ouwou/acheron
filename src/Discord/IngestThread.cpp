#include <QJsonDocument>
#include <QJsonParseError>

#include "IngestThread.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {

IngestThread::IngestThread(QObject *parent)
{
    memset(&stream, 0, sizeof(stream));
}

IngestThread::~IngestThread()
{
    stop();
    if (streamActive)
        inflateEnd(&stream);
}

void IngestThread::start()
{
    if (running) {
        qCDebug(LogNetwork) << "Attempt to start already running IngestThread";
        return;
    }
    running = true;
    thread = std::thread(&IngestThread::threadLoop, this);
}

void IngestThread::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }

    cv.notify_one();

    if (thread.joinable())
        thread.join();
}

void IngestThread::push(const QByteArray &data)
{
    {
        std::lock_guard lock(mutex);
        queue.push_back({ data, generation.load() });
    }

    cv.notify_one();
}

void IngestThread::reset()
{
    // Only touch queue (mutex-protected) and atomic generation.
    // Do NOT touch zlib state — that's owned by the worker thread.
    std::lock_guard lock(mutex);
    generation++;
    queue.clear();
}

void IngestThread::threadLoop()
{
    uint64_t activeGeneration = generation.load();

    while (true) {
        QByteArray data;
        uint64_t dataGen;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return !queue.empty() || !running; });
            if (!running)
                break;

            data = queue.front().data;
            dataGen = queue.front().generation;
            queue.pop_front();
        }

        // Generation changed — reset zlib state on the worker thread (safe, no race)
        if (dataGen != activeGeneration) {
            if (streamActive) {
                inflateEnd(&stream);
                streamActive = false;
            }
            memset(&stream, 0, sizeof(stream));
            decompressedBuffer.clear();
            activeGeneration = dataGen;
        }

        if (data.isEmpty())
            continue;

        if (!streamActive) {
            if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK) {
                qCWarning(LogNetwork) << "inflateInit2 failed";
                continue;
            }
            streamActive = true;
        }

        const auto len = data.size();
        const bool hasSuffix = len >= 4 && data[len - 4] == '\x00' && data[len - 3] == '\x00' &&
                               data[len - 2] == '\xFF' && data[len - 1] == '\xFF';

        stream.next_in = reinterpret_cast<Bytef *>(data.data());
        stream.avail_in = data.size();

        std::array<char, 32768> out;
        QByteArray decompressed;

        int ret;
        do {
            stream.avail_out = sizeof(out);
            stream.next_out = reinterpret_cast<Bytef *>(out.data());

            ret = inflate(&stream, Z_SYNC_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                qCWarning(LogNetwork) << "inflate failed:" << ret;
                inflateEnd(&stream);
                streamActive = false;
                break;
            }

            int have = sizeof(out) - stream.avail_out;
            decompressed.append(out.data(), have);
        } while (stream.avail_out == 0);

        if (!decompressed.isEmpty()) {
            decompressedBuffer.append(decompressed);

            if (hasSuffix) {
                QJsonParseError error;
                QJsonDocument doc = QJsonDocument::fromJson(decompressedBuffer, &error);
                decompressedBuffer.clear();
                if (error.error != QJsonParseError::NoError) {
                    qCWarning(LogNetwork) << "Failed to parse messages:" << error.errorString();
                } else if (doc.isObject()) {
                    emit payloadReceived(doc.object());
                }
            }
        }
    }
}

} // namespace Discord
} // namespace Acheron
