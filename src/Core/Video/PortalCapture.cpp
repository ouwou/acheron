#include "Core/Video/LinuxCapture.hpp"

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/CaptureCommon.hpp"
#include "Core/Media/PipeWireLibrary.hpp"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/macros.h>
}

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#endif

namespace Acheron {
namespace Core {
namespace Video {
namespace PortalCapture {

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)

namespace {

using Clock = std::chrono::steady_clock;
using Chosen = std::function<void(const CaptureSource &, const QString &)>;

const QLatin1String ID_PREFIX("pipewire:");
const QLatin1String PORTAL_SERVICE("org.freedesktop.portal.Desktop");
const QLatin1String PORTAL_PATH("/org/freedesktop/portal/desktop");
const QLatin1String SCREEN_CAST_INTERFACE("org.freedesktop.portal.ScreenCast");
const QLatin1String REQUEST_INTERFACE("org.freedesktop.portal.Request");
const QLatin1String SESSION_INTERFACE("org.freedesktop.portal.Session");
// For a call made on the UI thread to a portal that is there or is not. One that is only
// being started takes seconds, and is asked again no sooner than a couple of seconds on.
constexpr int PORTAL_PROBE_LIMIT_MS = 100;
constexpr auto PORTAL_ASKED_AGAIN_AFTER = std::chrono::seconds(2);
// For each call of the handshake that the user has no part in.
constexpr int PORTAL_ANSWER_LIMIT_MS = 5000;
constexpr auto START_LIMIT = std::chrono::seconds(5);

// org.freedesktop.portal.ScreenCast's source types, cursor modes and persist modes, and
// org.freedesktop.portal.Request's response codes.
constexpr uint MONITOR_SOURCES = 1;
constexpr uint WINDOW_SOURCES = 2;
constexpr uint CURSOR_IN_THE_PICTURE = 2;
constexpr uint PERSIST_UNTIL_REVOKED = 2;
constexpr uint VERSION_WITH_RESTORING = 4;
constexpr uint CALLED_OFF_BY_THE_USER = 1;

// drm_fourcc.h's DRM_FORMAT_MOD_LINEAR and DRM_FORMAT_MOD_INVALID. KWin drops the second
// when it is offered more than one.
constexpr uint64_t MODIFIERS[] = { 0, 0x00ffffffffffffffULL };

struct PixelLayout
{
    spa_video_format spa;
    AVPixelFormat av;
    uint32_t fourcc;
    const char *name;
};

// The fourccs are drm_fourcc.h's XRGB8888, ARGB8888, XBGR8888 and ABGR8888.
constexpr PixelLayout LAYOUTS[] = {
    { SPA_VIDEO_FORMAT_BGRx, AV_PIX_FMT_BGR0, MKTAG('X', 'R', '2', '4'), "BGRx" },
    { SPA_VIDEO_FORMAT_BGRA, AV_PIX_FMT_BGRA, MKTAG('A', 'R', '2', '4'), "BGRA" },
    { SPA_VIDEO_FORMAT_RGBx, AV_PIX_FMT_RGB0, MKTAG('X', 'B', '2', '4'), "RGBx" },
    { SPA_VIDEO_FORMAT_RGBA, AV_PIX_FMT_RGBA, MKTAG('A', 'B', '2', '4'), "RGBA" },
};

const PixelLayout *layoutOf(spa_video_format format)
{
    const auto found = std::find_if(std::begin(LAYOUTS), std::end(LAYOUTS), [format](const PixelLayout &layout) { return layout.spa == format; });
    return found == std::end(LAYOUTS) ? nullptr : found;
}

class PortalSession;

struct Source
{
    ~Source()
    {
        if (remote >= 0)
            ::close(remote);
    }

    quint32 node = 0;
    QSize size;
    // The portal's connection to PipeWire, where this process's own is not the one to use.
    int remote = -1;
    PortalSession *session = nullptr;
    std::atomic<bool> closed{ false };
};

struct Table
{
    std::mutex guard;
    QHash<QString, std::shared_ptr<Source>> sources;
    int nextNumber = 0;
};

Table &table()
{
    static Table table;
    return table;
}

QString registered(const std::shared_ptr<Source> &source)
{
    std::lock_guard<std::mutex> lock(table().guard);
    const QString id = ID_PREFIX + QString::number(table().nextNumber++);
    table().sources.insert(id, source);
    return id;
}

std::shared_ptr<Source> sourceOf(const QString &sourceId)
{
    std::lock_guard<std::mutex> lock(table().guard);
    return table().sources.value(sourceId);
}

struct Portal
{
    uint version = 0;
    uint sources = 0;
    uint cursors = 0;
};

Portal portal()
{
    static std::mutex guard;
    static std::optional<Portal> known;
    static std::optional<Clock::time_point> askedAt;

    std::lock_guard<std::mutex> lock(guard);
    if (known)
        return *known;
    if (askedAt && Clock::now() - *askedAt < PORTAL_ASKED_AGAIN_AFTER)
        return {};

    QDBusMessage question = QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    question << QString(SCREEN_CAST_INTERFACE);
    const QDBusReply<QVariantMap> answer = QDBusConnection::sessionBus().call(question, QDBus::Block, PORTAL_PROBE_LIMIT_MS);
    askedAt = Clock::now();
    if (!answer.isValid())
        return {};
    known = Portal{ answer.value().value(QStringLiteral("version")).toUInt(), answer.value().value(QStringLiteral("AvailableSourceTypes")).toUInt(), answer.value().value(QStringLiteral("AvailableCursorModes")).toUInt() };
    return *known;
}

bool pipeWireStarts()
{
    static const bool starts = [] {
        if (!Media::pipeWireCanBeCalled())
            return false;
        pw_init(nullptr, nullptr);
        pw_loop *loop = pw_loop_new(nullptr);
        if (loop)
            pw_loop_destroy(loop);
        return loop != nullptr;
    }();
    return starts;
}

QString uniqueToken()
{
    return QStringLiteral("acheron_%1").arg(QRandomGenerator::global()->generate());
}

struct PortalStream
{
    quint32 node = 0;
    QSize size;
};

PortalStream firstStreamOf(const QVariantMap &started)
{
    PortalStream stream;
    const QVariant listed = started.value(QStringLiteral("streams"));
    if (!listed.canConvert<QDBusArgument>())
        return stream;

    const QDBusArgument streams = listed.value<QDBusArgument>();
    streams.beginArray();
    if (!streams.atEnd()) {
        QVariantMap properties;
        streams.beginStructure();
        streams >> stream.node >> properties;
        streams.endStructure();

        const QVariant measured = properties.value(QStringLiteral("size"));
        if (measured.canConvert<QDBusArgument>()) {
            int width = 0;
            int height = 0;
            const QDBusArgument size = measured.value<QDBusArgument>();
            size.beginStructure();
            size >> width >> height;
            size.endStructure();
            stream.size = QSize(width, height);
        }
    }
    return stream;
}

class PortalSession : public QObject
{
    Q_OBJECT
public:
    PortalSession(CaptureSource::Kind kind, QString *token, QObject *receiver, Chosen chosen)
        : kind(kind), token(token), restored(token ? *token : QString()), receiver(receiver), chosen(std::move(chosen))
    {
        connect(receiver, &QObject::destroyed, this, [this] {
            if (!handle.isEmpty() && std::exchange(this->chosen, nullptr))
                close();
        });
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [this] { fail(captureText("The desktop did not answer when asked what to share.")); });
    }

    void begin()
    {
        ask(QStringLiteral("CreateSession"), {}, { { QStringLiteral("session_handle_token"), uniqueToken() } });
    }

    void close()
    {
        if (!handle.isEmpty())
            QDBusConnection::sessionBus().send(QDBusMessage::createMethodCall(PORTAL_SERVICE, handle, SESSION_INTERFACE, QStringLiteral("Close")));
        deleteLater();
    }

private slots:
    void answered(uint response, const QVariantMap &results)
    {
        deadline.stop();
        listenTo(QString());
        if (handle.isEmpty() && response == 0) {
            handle = results.value(QStringLiteral("session_handle")).toString();
            QDBusConnection::sessionBus().connect(PORTAL_SERVICE, handle, SESSION_INTERFACE, QStringLiteral("Closed"), this, SLOT(closedByTheDesktop()));
        }
        if (!receiver || response == CALLED_OFF_BY_THE_USER)
            return fail(QString());
        if (response != 0)
            return fail(captureText("The desktop did not hand anything over to share."));

        const QVariant session = QVariant::fromValue(QDBusObjectPath(handle));
        if (step == Step::CreateSession) {
            step = Step::SelectSources;
            ask(QStringLiteral("SelectSources"), { session }, sourceOptions());
        } else if (step == Step::SelectSources) {
            step = Step::Start;
            ask(QStringLiteral("Start"), { session, QString() }, {});
        } else {
            openRemote(firstStreamOf(results), results.value(QStringLiteral("restore_token")).toString());
        }
    }

    void closedByTheDesktop()
    {
        if (source)
            source->closed = true;
        else
            fail(captureText("The desktop ended the sharing before it began."));
    }

private:
    enum class Step {
        CreateSession,
        SelectSources,
        Start,
    };

    QVariantMap sourceOptions() const
    {
        QVariantMap options{ { QStringLiteral("types"), kind == CaptureSource::Kind::Window ? WINDOW_SOURCES : MONITOR_SOURCES }, { QStringLiteral("multiple"), false } };
        const Portal offered = portal();
        if (offered.cursors & CURSOR_IN_THE_PICTURE)
            options.insert(QStringLiteral("cursor_mode"), CURSOR_IN_THE_PICTURE);
        if (token && offered.version >= VERSION_WITH_RESTORING) {
            options.insert(QStringLiteral("persist_mode"), PERSIST_UNTIL_REVOKED);
            if (!restored.isEmpty())
                options.insert(QStringLiteral("restore_token"), restored);
        }
        return options;
    }

    void listenTo(const QString &path)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!request.isEmpty())
            bus.disconnect(PORTAL_SERVICE, request, REQUEST_INTERFACE, QStringLiteral("Response"), this, SLOT(answered(uint, QVariantMap)));
        request = path;
        if (!request.isEmpty())
            bus.connect(PORTAL_SERVICE, request, REQUEST_INTERFACE, QStringLiteral("Response"), this, SLOT(answered(uint, QVariantMap)));
    }

    void ask(const QString &method, QVariantList arguments, QVariantMap options)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        const QString requestToken = uniqueToken();
        const QString expected = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_')), requestToken);
        options.insert(QStringLiteral("handle_token"), requestToken);
        listenTo(expected);
        // The user takes as long as he likes over the dialog that Start shows.
        if (step != Step::Start)
            deadline.start(PORTAL_ANSWER_LIMIT_MS);

        QDBusMessage call = QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, SCREEN_CAST_INTERFACE, method);
        call.setArguments(arguments << options);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected](QDBusPendingCallWatcher *finished) {
            finished->deleteLater();
            const QDBusPendingReply<QDBusObjectPath> reply = *finished;
            if (reply.isError())
                fail(captureText("The desktop could not be asked what to share (%1).").arg(reply.error().message()));
            // A portal older than 0.9 names its requests itself.
            else if (request == expected && reply.value().path() != expected)
                listenTo(reply.value().path());
        });
    }

    void openRemote(const PortalStream &stream, const QString &nextToken)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, SCREEN_CAST_INTERFACE, QStringLiteral("OpenPipeWireRemote"));
        call << QVariant::fromValue(QDBusObjectPath(handle)) << QVariantMap();
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
        deadline.start(PORTAL_ANSWER_LIMIT_MS);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, stream, nextToken](QDBusPendingCallWatcher *finished) {
            deadline.stop();
            finished->deleteLater();
            const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *finished;
            const int remote = reply.isError() ? -1 : fcntl(reply.value().fileDescriptor(), F_DUPFD_CLOEXEC, 3);
            if (!receiver || !chosen || remote < 0 || stream.node == 0) {
                if (remote >= 0)
                    ::close(remote);
                return fail(receiver ? captureText("The desktop did not hand its pictures over.") : QString());
            }

            source = std::make_shared<Source>();
            source->node = stream.node;
            source->size = stream.size;
            source->remote = remote;
            source->session = this;

            CaptureSource picked;
            picked.id = registered(source);
            picked.kind = kind;
            picked.name = kind == CaptureSource::Kind::Window ? captureText("Window") : captureText("Screen");
            picked.geometry = QRect(QPoint(0, 0), stream.size);
            finish(picked, QString(), nextToken);
        });
    }

    void finish(const CaptureSource &picked, const QString &reason, const QString &nextToken)
    {
        const Chosen report = std::exchange(chosen, nullptr);
        if (!receiver)
            return;
        QMetaObject::invokeMethod(receiver.data(), [report, token = token, picked, reason, nextToken] {
            if (token && !picked.id.isEmpty())
                *token = nextToken;
            report(picked, reason);
        });
    }

    void fail(const QString &reason)
    {
        if (!chosen)
            return;
        deadline.stop();
        listenTo(QString());
        finish({}, reason, QString());
        close();
    }

    const CaptureSource::Kind kind;
    QString *const token;
    const QString restored;
    const QPointer<QObject> receiver;
    Chosen chosen;
    Step step = Step::CreateSession;
    QTimer deadline;
    QString request;
    QString handle;
    std::shared_ptr<Source> source;
};

using Media::FramePointer;

struct Pictures : std::enable_shared_from_this<Pictures>
{
    // Taken before the loop's own lock, which is taken before `guard`.
    std::mutex lifetime;
    // Null once the grabber is going.
    pw_thread_loop *loop = nullptr;
    std::mutex guard;
    std::condition_variable changed;
    pw_stream *stream = nullptr;
    spa_video_info_raw format = {};
    uint32_t memory = SPA_DATA_Invalid;
    int buffers = 0;
    bool takesCardBuffers = false;
    FramePointer newest;
    // Buffers kept from the stream while their pictures live, by the number they were kept
    // under: PipeWire uses a buffer's address again after a change of format.
    std::vector<std::pair<quint64, pw_buffer *>> kept;
    quint64 lastKept = 0;
    bool streamed = false;
    bool ended = false;
    QString error;
};

struct CardPicture
{
    std::shared_ptr<Pictures> pictures;
    // Zero for a picture whose buffer has gone back to the stream.
    quint64 number = 0;
    AVDRMFrameDescriptor layout = {};
};

CardPicture *cardPictureOf(const AVFrame *picture)
{
    return picture && picture->format == AV_PIX_FMT_DRM_PRIME ? static_cast<CardPicture *>(av_buffer_get_opaque(picture->buf[0])) : nullptr;
}

// With the loop's lock and the guard held.
void returnBuffer(Pictures *pictures, quint64 number)
{
    const auto found = std::find_if(pictures->kept.begin(), pictures->kept.end(), [number](const auto &kept) { return kept.first == number; });
    if (found == pictures->kept.end() || !pictures->stream)
        return;
    pw_stream_queue_buffer(pictures->stream, found->second);
    pictures->kept.erase(found);
}

void freeCardPicture(void *opaque, uint8_t *)
{
    auto *picture = static_cast<CardPicture *>(opaque);
    Pictures *pictures = picture->pictures.get();
    ::close(picture->layout.objects[0].fd);
    if (picture->number != 0) {
        // Before PipeWire 1.2 pw_loop_invoke is for one thread at a time, and the stream's own
        // thread uses it: the buffer goes back under the loop's lock, as thread-loop.h has it.
        std::lock_guard<std::mutex> alive(pictures->lifetime);
        if (pictures->loop) {
            pw_thread_loop_lock(pictures->loop);
            {
                std::lock_guard<std::mutex> lock(pictures->guard);
                returnBuffer(pictures, picture->number);
            }
            pw_thread_loop_unlock(pictures->loop);
        }
    }
    delete picture;
}

bool showsPicture(const spa_buffer *sent, const spa_video_info_raw &format)
{
    if (sent->n_datas == 0)
        return false;

    const spa_data &plane = sent->datas[0];
    const auto *header = static_cast<const spa_meta_header *>(spa_buffer_find_meta_data(sent, SPA_META_Header, sizeof(spa_meta_header)));
    if ((header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) || (plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) || plane.chunk->size == 0)
        return false;
    if (plane.type == SPA_DATA_DmaBuf)
        return plane.fd >= 0;

    const qint64 row = static_cast<qint64>(format.size.width) * 4;
    const qint64 needed = static_cast<qint64>(plane.chunk->stride) * (static_cast<qint64>(format.size.height) - 1) + row;
    return plane.data && plane.chunk->stride >= row && plane.chunk->size >= needed && plane.chunk->offset + needed <= plane.maxsize;
}

// The buffer's own descriptor is closed by PipeWire when the format changes or the stream
// ends, which a picture has to outlive.
bool keepOnCard(const spa_buffer *sent, const PixelLayout &layout, Pictures *pictures, AVFrame *picture)
{
    const int descriptor = fcntl(static_cast<int>(sent->datas[0].fd), F_DUPFD_CLOEXEC, 3);
    if (descriptor < 0)
        return false;
    auto *card = new CardPicture{ pictures->shared_from_this(), 0, {} };
    AVDRMFrameDescriptor &described = card->layout;
    const off_t size = lseek(descriptor, 0, SEEK_END);
    described.nb_objects = 1;
    described.objects[0].fd = descriptor;
    described.objects[0].size = size > 0 ? static_cast<size_t>(size) : sent->datas[0].maxsize;
    described.objects[0].format_modifier = pictures->format.modifier;
    described.nb_layers = 1;
    described.layers[0].format = layout.fourcc;
    // KWin's buffers for a consumer that keeps to its sync timeline end in two sync objects.
    for (uint32_t i = 0; i < sent->n_datas && described.layers[0].nb_planes < AV_DRM_MAX_PLANES; i++) {
        if (sent->datas[i].type == SPA_DATA_DmaBuf)
            described.layers[0].planes[described.layers[0].nb_planes++] = { 0, sent->datas[i].chunk->offset, sent->datas[i].chunk->stride };
    }

    picture->format = AV_PIX_FMT_DRM_PRIME;
    picture->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(&described), sizeof(described), freeCardPicture, card, AV_BUFFER_FLAG_READONLY);
    picture->data[0] = reinterpret_cast<uint8_t *>(&described);
    if (picture->buf[0])
        return true;
    ::close(descriptor);
    delete card;
    return false;
}

bool copyFromMemory(const spa_data &plane, const PixelLayout &layout, AVFrame *picture)
{
    picture->format = layout.av;
    if (av_frame_get_buffer(picture, 0) < 0)
        return false;
    const uint8_t *from = static_cast<const uint8_t *>(plane.data) + plane.chunk->offset;
    for (int y = 0; y < picture->height; y++)
        memcpy(picture->data[0] + static_cast<qsizetype>(y) * picture->linesize[0], from + static_cast<qsizetype>(y) * plane.chunk->stride, static_cast<size_t>(picture->width) * 4);
    return true;
}

// For PipeWire's thread, with the guard held.
FramePointer pictureIn(const spa_buffer *sent, Pictures *pictures)
{
    const spa_video_info_raw &format = pictures->format;
    const PixelLayout *layout = layoutOf(format.format);
    FramePointer picture(av_frame_alloc());
    if (!layout || !picture || !showsPicture(sent, format))
        return nullptr;

    picture->width = static_cast<int>(format.size.width);
    picture->height = static_cast<int>(format.size.height);
    const spa_data &plane = sent->datas[0];
    if (!(plane.type == SPA_DATA_DmaBuf ? keepOnCard(sent, *layout, pictures, picture.get()) : copyFromMemory(plane, *layout, picture.get())))
        return nullptr;

    const auto *crop = static_cast<const spa_meta_region *>(spa_buffer_find_meta_data(sent, SPA_META_VideoCrop, sizeof(spa_meta_region)));
    if (crop && spa_meta_region_is_valid(crop) && crop->region.position.x >= 0 && crop->region.position.y >= 0 && crop->region.position.x + crop->region.size.width <= format.size.width && crop->region.position.y + crop->region.size.height <= format.size.height) {
        picture->crop_left = static_cast<size_t>(crop->region.position.x);
        picture->crop_top = static_cast<size_t>(crop->region.position.y);
        picture->crop_right = format.size.width - crop->region.size.width - picture->crop_left;
        picture->crop_bottom = format.size.height - crop->region.size.height - picture->crop_top;
    }
    return picture;
}

void streamStateChanged(void *shared, pw_stream_state, pw_stream_state state, const char *error)
{
    auto *pictures = static_cast<Pictures *>(shared);
    std::lock_guard<std::mutex> lock(pictures->guard);
    if (state == PW_STREAM_STATE_STREAMING)
        pictures->streamed = true;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        pictures->ended = true;
        pictures->error = QString::fromUtf8(error);
    }
    pictures->changed.notify_all();
}

void streamFormatChanged(void *shared, uint32_t id, const spa_pod *param)
{
    auto *pictures = static_cast<Pictures *>(shared);
    spa_video_info_raw format = {};
    if (id != SPA_PARAM_Format || !param || spa_format_video_raw_parse(param, &format) < 0)
        return;
    {
        std::lock_guard<std::mutex> lock(pictures->guard);
        pictures->format = format;
    }

    uint8_t space[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, space, sizeof(space));
    const bool onCard = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
    const int memory = onCard ? 1 << SPA_DATA_DmaBuf : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    // Four is the most KWin gives, and the fewest of which the caller can keep a picture with
    // a newer one waiting behind it. Asked for alone first, since KWin's own choice is three.
    const spa_pod *params[] = {
        static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 4, 16), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(memory))),
        static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(memory))),
        static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
        static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)))),
    };
    pw_stream_update_params(pictures->stream, params, std::size(params));
}

void streamBufferAdded(void *shared, pw_buffer *buffer)
{
    auto *pictures = static_cast<Pictures *>(shared);
    std::lock_guard<std::mutex> lock(pictures->guard);
    pictures->buffers++;
    if (buffer->buffer->n_datas > 0)
        pictures->memory = buffer->buffer->datas[0].type;
    pictures->changed.notify_all();
}

void streamBufferRemoved(void *shared, pw_buffer *buffer)
{
    auto *pictures = static_cast<Pictures *>(shared);
    std::lock_guard<std::mutex> lock(pictures->guard);
    pictures->buffers--;
    pictures->kept.erase(std::remove_if(pictures->kept.begin(), pictures->kept.end(), [buffer](const auto &kept) { return kept.second == buffer; }), pictures->kept.end());
}

void streamHasPictures(void *shared)
{
    auto *pictures = static_cast<Pictures *>(shared);
    std::vector<FramePointer> passedOver;
    std::lock_guard<std::mutex> lock(pictures->guard);
    while (pw_buffer *buffer = pw_stream_dequeue_buffer(pictures->stream)) {
        FramePointer picture = pictureIn(buffer->buffer, pictures);
        CardPicture *card = cardPictureOf(picture.get());
        if (picture) {
            if (CardPicture *before = cardPictureOf(pictures->newest.get()))
                returnBuffer(pictures, std::exchange(before->number, 0));
            passedOver.push_back(std::exchange(pictures->newest, std::move(picture)));
        }
        // Two buffers are always left with the compositor: with one it waits for each picture's
        // buffer to come back before it can draw the next, and with none it stops.
        if (card && static_cast<int>(pictures->kept.size()) + 3 <= pictures->buffers) {
            card->number = ++pictures->lastKept;
            pictures->kept.emplace_back(card->number, buffer);
        } else {
            pw_stream_queue_buffer(pictures->stream, buffer);
        }
    }
    pictures->changed.notify_all();
}

struct Farewell
{
    pw_thread_loop *loop = nullptr;
    int awaited = -1;
    bool answered = false;
};

void coreAnswered(void *leaving, uint32_t id, int seq)
{
    auto *farewell = static_cast<Farewell *>(leaving);
    if (id != PW_ID_CORE || seq != farewell->awaited)
        return;
    farewell->answered = true;
    pw_thread_loop_signal(farewell->loop, false);
}

const pw_core_events CORE_EVENTS = {
    .version = PW_VERSION_CORE_EVENTS,
    .done = coreAnswered,
};

const pw_stream_events STREAM_EVENTS = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = streamStateChanged,
    .param_changed = streamFormatChanged,
    .add_buffer = streamBufferAdded,
    .remove_buffer = streamBufferRemoved,
    .process = streamHasPictures,
};

const spa_pod *offerOf(spa_pod_builder *builder, const PixelLayout &layout, int framerate, bool onCard)
{
    spa_rectangle usual = { 1920, 1080 };
    spa_rectangle smallest = { 1, 1 };
    spa_rectangle largest = { 16384, 16384 };
    spa_fraction wanted = { static_cast<uint32_t>(framerate), 1 };
    spa_fraction unsteady = { 0, 1 };
    spa_fraction slowest = { 1, 1 };
    spa_fraction fastest = { 1000, 1 };

    spa_pod_frame object;
    spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(layout.spa), 0);
    if (onCard) {
        spa_pod_frame choice;
        spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(builder, static_cast<int64_t>(MODIFIERS[0]));
        for (const uint64_t modifier : MODIFIERS)
            spa_pod_builder_long(builder, static_cast<int64_t>(modifier));
        spa_pod_builder_pop(builder, &choice);
    }
    spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&usual, &smallest, &largest), SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&unsteady, &unsteady, &fastest), SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&wanted, &slowest, &wanted), 0);
    return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &object));
}

const char *memoryName(uint32_t memory)
{
    switch (memory) {
    case SPA_DATA_DmaBuf:
        return "dmabuf";
    case SPA_DATA_MemFd:
        return "memfd";
    case SPA_DATA_MemPtr:
        return "memptr";
    }
    return nullptr;
}

class Grabber : public LinuxGrabber
{
public:
    Grabber(std::shared_ptr<Source> from, int framerate, bool onCard)
        : source(std::move(from)), interval(std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / std::max(1, framerate))
    {
        loop = pw_thread_loop_new("acheron-capture", nullptr);
        context = loop ? pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0) : nullptr;
        if (!context || pw_thread_loop_start(loop) < 0)
            return;

        pw_thread_loop_lock(loop);
        pictures->loop = loop;
        pictures->takesCardBuffers = onCard;
        farewell.loop = loop;
        core = source->remote >= 0 ? pw_context_connect_fd(context, fcntl(source->remote, F_DUPFD_CLOEXEC, 3), nullptr, 0) : pw_context_connect(context, nullptr, 0);
        if (core)
            pw_core_add_listener(core, &coreListener, &CORE_EVENTS, &farewell);
        // node.dont-fallback has WirePlumber end a stream whose target is gone, and not look
        // for another source to link it to.
        stream = core ? pw_stream_new(core, "acheron", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", "node.dont-fallback", "true", nullptr)) : nullptr;
        if (stream) {
            pictures->stream = stream;
            pw_stream_add_listener(stream, &listener, &STREAM_EVENTS, pictures.get());

            uint8_t space[4096];
            spa_pod_builder builder;
            spa_pod_builder_init(&builder, space, sizeof(space));
            std::vector<const spa_pod *> offers;
            for (const bool card : { true, false }) {
                for (const PixelLayout &layout : LAYOUTS) {
                    if (onCard || !card)
                        offers.push_back(offerOf(&builder, layout, std::max(1, framerate), card));
                }
            }
            const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT);
            connected = pw_stream_connect(stream, PW_DIRECTION_INPUT, source->node, flags, offers.data(), static_cast<uint32_t>(offers.size())) >= 0;
        }
        pw_thread_loop_unlock(loop);
    }

    ~Grabber() override
    {
        FramePointer unclaimed;
        {
            std::lock_guard<std::mutex> alive(pictures->lifetime);
            pictures->loop = nullptr;
        }
        if (core) {
            pw_thread_loop_lock(loop);
            if (stream)
                pw_stream_destroy(stream);
            {
                std::lock_guard<std::mutex> lock(pictures->guard);
                pictures->stream = nullptr;
                unclaimed = std::move(pictures->newest);
            }
            // The portal's one connection is used again by the next grabber, which would take
            // what the server still had to say to this one for its own: it is heard out first.
            farewell.awaited = pw_core_sync(core, PW_ID_CORE, 0);
            int waited = farewell.awaited < 0 ? farewell.awaited : 0;
            while (!farewell.answered && waited == 0)
                waited = pw_thread_loop_timed_wait(loop, 1);
            pw_thread_loop_unlock(loop);
        }
        if (loop)
            pw_thread_loop_stop(loop);
        if (core)
            pw_core_disconnect(core);
        if (context)
            pw_context_destroy(context);
        if (loop)
            pw_thread_loop_destroy(loop);
    }

    Result grab(AVFrame *into, std::chrono::milliseconds wait) override
    {
        const auto deadline = Clock::now() + wait;
        std::unique_lock<std::mutex> lock(pictures->guard);
        for (;;) {
            const auto now = Clock::now();
            if (source->closed || (pictures->ended && pictures->streamed))
                return Result::Closed;
            // A stream's first consumer settles its buffers for every other, whatever those offer.
            if (pictures->memory == SPA_DATA_DmaBuf && !pictures->takesCardBuffers) {
                failureText = captureText("Another program has the desktop keep these pictures on the graphics card.");
                return Result::Failed;
            }
            if (!connected || pictures->ended || (!pictures->streamed && now - opened > START_LIMIT)) {
                failureText = pictures->error.isEmpty() ? captureText("The desktop is not sending its pictures.") : captureText("The desktop is not sending its pictures (%1).").arg(pictures->error);
                return Result::Failed;
            }
            if (pictures->newest && now >= due)
                break;
            if (now >= deadline)
                return Result::NoFrame;
            pictures->changed.wait_until(lock, pictures->newest ? std::min(deadline, due) : deadline);
        }

        const FramePointer picture = std::move(pictures->newest);
        due = Clock::now() + interval;
        lock.unlock();

        av_frame_unref(into);
        av_frame_move_ref(into, picture.get());
        std::lock_guard<std::mutex> listed(table().guard);
        source->size = QSize(into->width - static_cast<int>(into->crop_left + into->crop_right), into->height - static_cast<int>(into->crop_top + into->crop_bottom));
        return Result::Frame;
    }

    bool deliversOnlyChanges() const override { return true; }

    QString name() const override
    {
        std::lock_guard<std::mutex> lock(pictures->guard);
        const PixelLayout *layout = layoutOf(pictures->format.format);
        const char *memory = memoryName(pictures->memory);
        return layout && memory ? QStringLiteral("pipewire %1 %2").arg(QLatin1String(memory), QLatin1String(layout->name)) : QStringLiteral("pipewire");
    }

    QString failure() const override { return failureText; }

private:
    const std::shared_ptr<Source> source;
    const std::shared_ptr<Pictures> pictures = std::make_shared<Pictures>();
    const Clock::duration interval;
    const Clock::time_point opened = Clock::now();
    Clock::time_point due;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener = {};
    spa_hook coreListener = {};
    Farewell farewell;
    bool connected = false;
    QString failureText;
};

} // namespace

bool usable()
{
    return (portal().sources & (MONITOR_SOURCES | WINDOW_SOURCES)) != 0 && pipeWireStarts();
}

void choose(CaptureSource::Kind kind, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen)
{
    chooseAgain(kind, nullptr, receiver, std::move(chosen));
}

void chooseAgain(CaptureSource::Kind kind, QString *token, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen)
{
    (new PortalSession(kind, token, receiver, std::move(chosen)))->begin();
}

CaptureSource adopt(quint32 node, const QSize &size)
{
    auto source = std::make_shared<Source>();
    source->node = node;
    source->size = size;

    CaptureSource adopted;
    adopted.id = registered(source);
    adopted.geometry = QRect(QPoint(0, 0), size);
    return adopted;
}

QSize sizeOf(const CaptureSource &source)
{
    std::lock_guard<std::mutex> lock(table().guard);
    const std::shared_ptr<Source> known = table().sources.value(source.id);
    return known ? known->size : QSize();
}

std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate, bool onCard)
{
    std::shared_ptr<Source> source = sourceOf(sourceId);
    if (!source || !pipeWireStarts())
        return nullptr;
    return std::make_unique<Grabber>(std::move(source), framerate, onCard);
}

void release(const QString &sourceId)
{
    std::shared_ptr<Source> source;
    {
        std::lock_guard<std::mutex> lock(table().guard);
        source = table().sources.take(sourceId);
    }
    if (!source)
        return;
    source->closed = true;
    if (source->session)
        QMetaObject::invokeMethod(source->session, &PortalSession::close, Qt::QueuedConnection);
}

#else

bool usable()
{
    return false;
}

void choose(CaptureSource::Kind, QObject *, std::function<void(const CaptureSource &, const QString &)>)
{
}

void chooseAgain(CaptureSource::Kind, QString *, QObject *, std::function<void(const CaptureSource &, const QString &)>)
{
}

CaptureSource adopt(quint32, const QSize &)
{
    return {};
}

QSize sizeOf(const CaptureSource &)
{
    return {};
}

std::unique_ptr<LinuxGrabber> open(const QString &, int, bool)
{
    return nullptr;
}

void release(const QString &)
{
}

#endif

} // namespace PortalCapture
} // namespace Video
} // namespace Core
} // namespace Acheron

#if defined(ACHERON_HAVE_PIPEWIRE) && defined(ACHERON_HAVE_DBUS)
#include "PortalCapture.moc"
#endif
