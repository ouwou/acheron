#include "CurlAvio.hpp"

#ifdef ACHERON_HAVE_FFMPEG

#include <QByteArray>
#include <QUrl>

#include <curl/curl.h>

#include <algorithm>
#include <deque>
#include <string>

#include "Core/Logging.hpp"
#include "Discord/CurlUtils.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace Acheron {
namespace Core {
namespace Media {
namespace CurlAvio {

namespace {

constexpr int BUFFER_SIZE = 256 * 1024;
constexpr size_t PENDING_HIGH_WATER = 4 * 1024 * 1024;
constexpr int POLL_INTERVAL_MS = 100;
constexpr int STALL_TIMEOUT_MS = 15000;

struct Stream
{
    std::string url;
    ProxyConfig proxy;
    AVIOInterruptCB interrupt{};

    CURLM *multi = nullptr;
    CURL *easy = nullptr;

    std::deque<char> pending;
    qint64 position = 0;
    qint64 contentLength = -1;
    bool transferRunning = false;
    bool paused = false;
    bool endOfStream = false;
    bool failed = false;

    ~Stream()
    {
        stopTransfer();
        if (multi)
            curl_multi_cleanup(multi);
    }

    void stopTransfer()
    {
        if (easy) {
            if (multi)
                curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            easy = nullptr;
        }
        transferRunning = false;
        paused = false;
    }

    bool interrupted() const
    {
        return interrupt.callback && interrupt.callback(interrupt.opaque);
    }
};

size_t onHeader(char *buffer, size_t size, size_t count, void *userdata)
{
    auto *stream = static_cast<Stream *>(userdata);
    const QByteArray header = QByteArray(buffer, static_cast<int>(size * count)).trimmed().toLower();

    bool ok = false;
    if (header.startsWith("content-range:")) {
        const qint64 total = header.mid(header.lastIndexOf('/') + 1).trimmed().toLongLong(&ok);
        if (ok)
            stream->contentLength = total;
    } else if (stream->position == 0 && header.startsWith("content-length:")) {
        const qint64 length = header.mid(header.indexOf(':') + 1).trimmed().toLongLong(&ok);
        if (ok)
            stream->contentLength = length;
    }

    return size * count;
}

size_t onData(char *buffer, size_t size, size_t count, void *userdata)
{
    auto *stream = static_cast<Stream *>(userdata);
    const size_t bytes = size * count;

    if (stream->pending.size() >= PENDING_HIGH_WATER) {
        stream->paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }

    stream->pending.insert(stream->pending.end(), buffer, buffer + bytes);
    return bytes;
}

bool startTransfer(Stream *stream)
{
    stream->stopTransfer();
    stream->endOfStream = false;

    stream->easy = curl_easy_init();
    if (!stream->easy)
        return false;

    Discord::CurlUtils::applyCommonOptions(stream->easy);
    Discord::CurlUtils::applyProxy(stream->easy, stream->proxy);

    curl_easy_setopt(stream->easy, CURLOPT_URL, stream->url.c_str());
    curl_easy_setopt(stream->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(stream->easy, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(stream->easy, CURLOPT_WRITEDATA, stream);
    curl_easy_setopt(stream->easy, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(stream->easy, CURLOPT_HEADERDATA, stream);
    curl_easy_setopt(stream->easy, CURLOPT_CONNECTTIMEOUT, 15L);

    if (stream->position > 0) {
        const std::string range = std::to_string(stream->position) + "-";
        curl_easy_setopt(stream->easy, CURLOPT_RANGE, range.c_str());
    }

    if (curl_multi_add_handle(stream->multi, stream->easy) != CURLM_OK) {
        stream->stopTransfer();
        return false;
    }

    stream->transferRunning = true;
    return true;
}

void resumeIfDrained(Stream *stream)
{
    if (!stream->paused || stream->pending.size() >= PENDING_HIGH_WATER)
        return;

    stream->paused = false;
    curl_easy_pause(stream->easy, CURLPAUSE_CONT);
}

// Runs the transfer until at least one byte is buffered, the body ends, or we
// are interrupted. Returns false on hard failure.
bool fillBuffer(Stream *stream)
{
    int idleMs = 0;

    while (stream->pending.empty() && stream->transferRunning) {
        if (stream->interrupted())
            return false;

        int running = 0;
        if (curl_multi_perform(stream->multi, &running) != CURLM_OK)
            return false;

        if (running == 0) {
            stream->transferRunning = false;
            stream->endOfStream = true;

            int queued = 0;
            while (CURLMsg *msg = curl_multi_info_read(stream->multi, &queued)) {
                if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK) {
                    qCWarning(LogVideo) << "media transfer failed:" << curl_easy_strerror(msg->data.result);
                    stream->failed = true;
                }
            }
            break;
        }

        if (!stream->pending.empty())
            break;

        const size_t before = stream->pending.size();
        int ready = 0;
        curl_multi_poll(stream->multi, nullptr, 0, POLL_INTERVAL_MS, &ready);

        if (stream->pending.size() == before) {
            idleMs += POLL_INTERVAL_MS;
            if (idleMs >= STALL_TIMEOUT_MS) {
                qCWarning(LogVideo) << "media transfer stalled";
                return false;
            }
        } else {
            idleMs = 0;
        }
    }

    return !stream->failed;
}

int readPacket(void *opaque, uint8_t *buf, int size)
{
    auto *stream = static_cast<Stream *>(opaque);

    if (stream->interrupted())
        return AVERROR_EXIT;

    if (stream->pending.empty()) {
        if (!stream->transferRunning && !stream->endOfStream && !startTransfer(stream))
            return AVERROR(EIO);

        if (!fillBuffer(stream))
            return stream->interrupted() ? AVERROR_EXIT : AVERROR(EIO);
    }

    if (stream->pending.empty())
        return AVERROR_EOF;

    const int copied = static_cast<int>(qMin<size_t>(static_cast<size_t>(size), stream->pending.size()));
    std::copy(stream->pending.begin(), stream->pending.begin() + copied, buf);
    stream->pending.erase(stream->pending.begin(), stream->pending.begin() + copied);
    stream->position += copied;

    resumeIfDrained(stream);

    return copied;
}

int64_t seekPacket(void *opaque, int64_t offset, int whence)
{
    auto *stream = static_cast<Stream *>(opaque);

    if (whence == AVSEEK_SIZE)
        return stream->contentLength >= 0 ? stream->contentLength : AVERROR(ENOSYS);

    qint64 target = offset;
    if (whence == SEEK_CUR)
        target = stream->position + offset;
    else if (whence == SEEK_END)
        target = stream->contentLength >= 0 ? stream->contentLength + offset : -1;
    else if (whence != SEEK_SET)
        return AVERROR(EINVAL);

    if (target < 0)
        return AVERROR(EINVAL);

    if (target == stream->position)
        return target;

    stream->stopTransfer();
    stream->pending.clear();
    stream->endOfStream = false;
    stream->failed = false;
    stream->position = target;

    return target;
}

} // namespace

bool handlesScheme(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

AVIOContext *create(const QString &url, const ProxyConfig &proxy, const AVIOInterruptCB &interrupt)
{
    auto *stream = new Stream;
    stream->url = url.toStdString();
    stream->proxy = proxy;
    stream->interrupt = interrupt;
    stream->multi = curl_multi_init();

    if (!stream->multi) {
        delete stream;
        return nullptr;
    }

    auto *buffer = static_cast<unsigned char *>(av_malloc(BUFFER_SIZE));
    if (!buffer) {
        delete stream;
        return nullptr;
    }

    AVIOContext *context = avio_alloc_context(buffer, BUFFER_SIZE, 0, stream, readPacket, nullptr, seekPacket);
    if (!context) {
        av_free(buffer);
        delete stream;
        return nullptr;
    }

    return context;
}

void destroy(AVIOContext *context)
{
    if (!context)
        return;

    delete static_cast<Stream *>(context->opaque);

    av_freep(&context->buffer);
    avio_context_free(&context);
}

} // namespace CurlAvio
} // namespace Media
} // namespace Core
} // namespace Acheron

#endif // ACHERON_HAVE_FFMPEG
