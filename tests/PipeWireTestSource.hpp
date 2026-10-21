#pragma once

#include "Core/Video/LinuxCapture.hpp"

#include <QElapsedTimer>
#include <QSettings>
#include <QSize>
#include <QTest>
#include <QThread>

#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

class PipeWireTestSource
{
public:
    // as KWin: it would rather make three, and no fewer than two
    static constexpr int BUFFERS = 4;
    static constexpr uint64_t LINEAR_MODIFIER = 0;
    // SPA_DATA_SyncObj came with PipeWire 1.2
    static constexpr bool HAS_SYNC_OBJECTS = PW_CHECK_VERSION(1, 2, 0);

    enum class Spoiling {
        None,
        MarkedInHeader,
        MarkedInChunk,
        Empty,
    };
    using Painter = std::function<void(void *into, const QSize &size, int stride, quint32 number, bool again)>;

    [[nodiscard]] static bool cardMemoryCanBeMade()
    {
        const int device = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (device >= 0)
            ::close(device);
        return device >= 0;
    }

    // two sync objects per card buffer, as KWin gives a consumer that keeps to its sync timeline
    PipeWireTestSource(const QSize &size, Painter paint, spa_video_format format = SPA_VIDEO_FORMAT_BGRx, bool offersCard = false, bool syncObjects = false)
        : size(size), paint(std::move(paint)), format(format), offersCard(offersCard), syncObjects(syncObjects)
    {
        pw_init(nullptr, nullptr);
        loop = pw_thread_loop_new("acheron-test-source", nullptr);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        pw_thread_loop_start(loop);

        // a machine may have libpipewire and none of PipeWire's own files, and then no context
        Locked lock(loop);
        core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
        if (!core)
            return;
        stream = pw_stream_new(core, "acheron-test-source", nullptr);
        pw_stream_add_listener(stream, &listener, &EVENTS, this);
        uint8_t space[1024];
        std::vector<const spa_pod *> offered = offers(space, sizeof(space));
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);
        pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, offered.data(), static_cast<uint32_t>(offered.size()));
    }

    ~PipeWireTestSource()
    {
        pw_thread_loop_stop(loop);
        if (stream)
            pw_stream_destroy(stream);
        if (core)
            pw_core_disconnect(core);
        if (context)
            pw_context_destroy(context);
        pw_thread_loop_destroy(loop);
    }

    [[nodiscard]] quint32 node()
    {
        quint32 id = PW_ID_ANY;
        for (int tries = 0; stream && id == PW_ID_ANY && tries < 500; tries++) {
            QThread::msleep(10);
            Locked lock(loop);
            id = pw_stream_get_node_id(stream);
        }
        return id;
    }

    [[nodiscard]] QSize agreedSize() const { return QSize(agreedWidth, agreedHeight); }
    [[nodiscard]] bool agreedOnCard() const { return onCard; }
    [[nodiscard]] int buffers() const { return made; }

    bool send(quint32 number, Spoiling spoiling = Spoiling::None, const QRect &crop = QRect())
    {
        for (int tries = 0; carrying && tries < 400; tries++)
            QThread::usleep(250);

        Locked lock(loop);
        pw_buffer *buffer = streaming ? pw_stream_dequeue_buffer(stream) : nullptr;
        if (!buffer)
            return false;

        auto *own = static_cast<Memory *>(buffer->user_data);
        const int stride = agreedWidth * 4;
        paint(own->mapped, agreedSize(), stride, number, std::exchange(own->painted, true));

        spa_chunk *chunk = buffer->buffer->datas[0].chunk;
        chunk->offset = 0;
        chunk->stride = stride;
        chunk->size = spoiling == Spoiling::Empty ? 0 : static_cast<uint32_t>(stride * agreedHeight);
        chunk->flags = spoiling == Spoiling::MarkedInChunk ? SPA_CHUNK_FLAG_CORRUPTED : SPA_CHUNK_FLAG_NONE;
        if (auto *header = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(spa_meta_header))))
            header->flags = spoiling == Spoiling::MarkedInHeader ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
        if (auto *region = static_cast<spa_meta_region *>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)))) {
            region->region.position = { crop.x(), crop.y() };
            region->region.size = { static_cast<uint32_t>(crop.width()), static_cast<uint32_t>(crop.height()) };
        }

        pw_stream_queue_buffer(stream, buffer);
        carrying = true;
        pw_stream_trigger_process(stream);
        return true;
    }

    bool sendWhenItCan(quint32 number, Spoiling spoiling = Spoiling::None, const QRect &crop = QRect())
    {
        for (int tries = 0; tries < 1000; tries++) {
            if (send(number, spoiling, crop))
                return true;
            QThread::msleep(5);
        }
        return false;
    }

    void resize(const QSize &wanted)
    {
        Locked lock(loop);
        size = wanted;
        uint8_t space[1024];
        std::vector<const spa_pod *> offered = offers(space, sizeof(space));
        pw_stream_update_params(stream, offered.data(), static_cast<uint32_t>(offered.size()));
    }

private:
    struct Locked
    {
        explicit Locked(pw_thread_loop *loop) : loop(loop) { pw_thread_loop_lock(loop); }
        ~Locked() { pw_thread_loop_unlock(loop); }
        pw_thread_loop *loop;
    };

    struct Memory
    {
        void *mapped = MAP_FAILED;
        size_t bytes = 0;
        int descriptor = -1;
        bool painted = false;
    };

    const spa_pod *offer(spa_pod_builder *builder, bool card, bool settled) const
    {
        spa_rectangle pictures = { static_cast<uint32_t>(size.width()), static_cast<uint32_t>(size.height()) };
        spa_fraction unsteady = { 0, 1 };
        spa_pod_frame object;
        spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
        if (card) {
            spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, settled ? SPA_POD_PROP_FLAG_MANDATORY : SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            if (settled) {
                spa_pod_builder_long(builder, LINEAR_MODIFIER);
            } else {
                spa_pod_frame choice;
                spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
                spa_pod_builder_long(builder, LINEAR_MODIFIER);
                spa_pod_builder_long(builder, LINEAR_MODIFIER);
                spa_pod_builder_pop(builder, &choice);
            }
        }
        spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&pictures), SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&unsteady), 0);
        return static_cast<const spa_pod *>(spa_pod_builder_pop(builder, &object));
    }

    // as KWin: the settled modifier first once one was asked for, then every modifier, then plain memory
    std::vector<const spa_pod *> offers(uint8_t *space, uint32_t bytes) const
    {
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, space, bytes);
        std::vector<const spa_pod *> offered;
        if (offersCard && settling)
            offered.push_back(offer(&builder, true, true));
        if (offersCard)
            offered.push_back(offer(&builder, true, false));
        offered.push_back(offer(&builder, false, false));
        return offered;
    }

    static void stateChanged(void *self, pw_stream_state, pw_stream_state state, const char *)
    {
        static_cast<PipeWireTestSource *>(self)->streaming = state == PW_STREAM_STATE_STREAMING;
    }

    static void carried(void *self)
    {
        static_cast<PipeWireTestSource *>(self)->carrying = false;
    }

    static void formatChanged(void *self, uint32_t id, const spa_pod *param)
    {
        auto *source = static_cast<PipeWireTestSource *>(self);
        spa_video_info_raw agreed = {};
        if (id != SPA_PARAM_Format || !param || spa_format_video_raw_parse(param, &agreed) < 0)
            return;

        uint8_t space[1024];
        const spa_pod_prop *modifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (modifier && (modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
            source->settling = true;
            std::vector<const spa_pod *> offered = source->offers(space, sizeof(space));
            pw_stream_update_params(source->stream, offered.data(), static_cast<uint32_t>(offered.size()));
            return;
        }

        source->onCard = modifier != nullptr;
        source->agreedWidth = static_cast<int>(agreed.size.width);
        source->agreedHeight = static_cast<int>(agreed.size.height);
        const int stride = source->agreedWidth * 4;
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, space, sizeof(space));
        const spa_pod *params[] = {
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, BUFFERS), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(modifier && source->syncObjects && HAS_SYNC_OBJECTS ? 3 : 1), SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * source->agreedHeight), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << (modifier ? SPA_DATA_DmaBuf : SPA_DATA_MemFd)))),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)))),
        };
        pw_stream_update_params(source->stream, params, std::size(params));
    }

    static void bufferAdded(void *self, pw_buffer *buffer)
    {
        auto *source = static_cast<PipeWireTestSource *>(self);
        auto *own = new Memory;
        const long page = sysconf(_SC_PAGESIZE);
        own->bytes = static_cast<size_t>((static_cast<long>(source->agreedWidth) * 4 * source->agreedHeight + page - 1) / page * page);
        const int shared = memfd_create("acheron-test-picture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (ftruncate(shared, static_cast<off_t>(own->bytes)) == 0)
            own->mapped = mmap(nullptr, own->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0);
        own->descriptor = shared;

        spa_data &plane = buffer->buffer->datas[0];
        plane.type = SPA_DATA_MemFd;
        plane.flags = SPA_DATA_FLAG_READABLE;
#ifdef SPA_DATA_FLAG_MAPPABLE
        plane.flags |= SPA_DATA_FLAG_MAPPABLE;
#endif
        if (source->onCard) {
            // the kernel makes a card buffer only of memory that cant shrink
            fcntl(shared, F_ADD_SEALS, F_SEAL_SHRINK);
            const int device = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            udmabuf_create wanted = {};
            wanted.memfd = static_cast<uint32_t>(shared);
            wanted.flags = UDMABUF_FLAGS_CLOEXEC;
            wanted.size = own->bytes;
            own->descriptor = ioctl(device, UDMABUF_CREATE, &wanted);
            ::close(device);
            ::close(shared);
            plane.type = SPA_DATA_DmaBuf;
            plane.flags = SPA_DATA_FLAG_READWRITE;
        }
        plane.fd = own->descriptor;
        plane.mapoffset = 0;
        plane.maxsize = static_cast<uint32_t>(own->bytes);
        plane.data = nullptr;
#if PW_CHECK_VERSION(1, 2, 0)
        for (uint32_t i = 1; i < buffer->buffer->n_datas; i++) {
            spa_data &sync = buffer->buffer->datas[i];
            sync.type = SPA_DATA_SyncObj;
            sync.flags = SPA_DATA_FLAG_READWRITE;
            sync.fd = fcntl(own->descriptor, F_DUPFD_CLOEXEC, 3);
            sync.data = nullptr;
            sync.chunk->offset = 4096;
            sync.chunk->stride = 4;
        }
#endif
        buffer->user_data = own;
        source->made++;
    }

    static void bufferRemoved(void *self, pw_buffer *buffer)
    {
        auto *own = static_cast<Memory *>(buffer->user_data);
        if (own->mapped != MAP_FAILED)
            munmap(own->mapped, own->bytes);
        ::close(own->descriptor);
        for (uint32_t i = 1; i < buffer->buffer->n_datas; i++)
            ::close(static_cast<int>(buffer->buffer->datas[i].fd));
        delete own;
        static_cast<PipeWireTestSource *>(self)->made--;
    }

    static constexpr pw_stream_events EVENTS = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = stateChanged,
        .param_changed = formatChanged,
        .add_buffer = bufferAdded,
        .remove_buffer = bufferRemoved,
        .trigger_done = carried,
    };

    QSize size;
    const Painter paint;
    const spa_video_format format;
    const bool offersCard;
    const bool syncObjects;
    bool settling = false;
    std::atomic<bool> streaming{ false };
    std::atomic<bool> carrying{ false };
    std::atomic<bool> onCard{ false };
    std::atomic<int> agreedWidth{ 0 };
    std::atomic<int> agreedHeight{ 0 };
    std::atomic<int> made{ 0 };
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener = {};
};

// the screen the machine's owner approved once for the tests, without the dialog. the approval is at
// ~/.config/acheron-tests/portal.conf
struct ApprovedScreen
{
    enum class State {
        NeverApproved,
        NoLongerApproved,
        Restored,
    };

    static constexpr char NEVER_APPROVED[] = "No screen has been approved for these tests: run TestPortalCapture -choose-screen once and answer the desktop's dialog, ticking that the choice may be restored.";

    static void remember(const QString &token)
    {
        QSettings approvals(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("acheron-tests"), QStringLiteral("portal"));
        if (token.isEmpty())
            approvals.remove(QStringLiteral("screen_token"));
        else
            approvals.setValue(QStringLiteral("screen_token"), token);
    }

    [[nodiscard]] static ApprovedScreen restore()
    {
        using namespace Acheron::Core::Video;
        ApprovedScreen screen;
        QString token = QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("acheron-tests"), QStringLiteral("portal")).value(QStringLiteral("screen_token")).toString();
        if (token.isEmpty())
            return screen;

        screen.state = State::NoLongerApproved;
        screen.problem = QStringLiteral("The desktop asked again instead of restoring the approved screen.");
        QElapsedTimer waited;
        waited.start();
        {
            QObject receiver;
            bool answered = false;
            PortalCapture::chooseAgain(CaptureSource::Kind::Screen, &token, &receiver, [&](const CaptureSource &source, const QString &why) {
                screen.source = source;
                screen.problem = why;
                screen.answeredAfterMs = waited.elapsed();
                answered = true;
            });
            while (!answered && waited.elapsed() < 4000)
                QTest::qWait(20);
        }
        if (screen.source.id.isEmpty()) {
            remember(QString());
            screen.problem += QStringLiteral(" The stored approval was dropped: run TestPortalCapture -choose-screen again.");
            return screen;
        }
        remember(token);
        screen.state = State::Restored;
        return screen;
    }

    State state = State::NeverApproved;
    Acheron::Core::Video::CaptureSource source;
    QString problem;
    qint64 answeredAfterMs = 0;
};
