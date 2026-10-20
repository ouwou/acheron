#include "Core/Audio/LoopbackCapture.hpp"

#if !defined(Q_OS_WIN) && defined(ACHERON_HAVE_PIPEWIRE)

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"
#include "Core/Media/PipeWireLibrary.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QMap>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/utils/result.h>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

using Target = LoopbackCapture::Target;

constexpr int ANSWER_TIMEOUT_MS = 200;
constexpr int ASKED_AGAIN_AFTER_MS = 1000;
constexpr spa_audio_channel CAPTURED[] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR };
enum Captured { FL,
                FR,
                FC,
                LFE,
                SL,
                SR,
                RL,
                RR,
                CAPTURED_CHANNELS };

QString failureText(const QString &detail)
{
    return LoopbackCapture::tr("PipeWire could not capture the sound (%1).").arg(detail);
}

bool initPipeWire()
{
    if (!Media::pipeWireCanBeCalled())
        return false;
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
    return true;
}

quint32 parentOf(quint32 processId)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(processId));
    if (!stat.open(QIODevice::ReadOnly))
        return 0;
    // The name comes second, in brackets, and may itself hold brackets and spaces.
    const QByteArray line = stat.readAll();
    return line.mid(line.lastIndexOf(')') + 2).split(' ').value(1).toUInt();
}

bool isInTree(quint32 processId, quint32 root)
{
    for (; processId != 0; processId = parentOf(processId)) {
        if (processId == root)
            return true;
    }
    return false;
}

class Graph
{
public:
    struct Client
    {
        pw_proxy *proxy = nullptr;
        spa_hook listener = {};
        bool known = false;
        quint32 processId = 0;
        QString name;
        std::optional<bool> inTree;
    };
    struct Node
    {
        Graph *graph = nullptr;
        pw_proxy *proxy = nullptr;
        spa_hook listener = {};
        std::optional<quint32> client;
        pw_node_state state = PW_NODE_STATE_CREATING;
        bool known = false;
        bool chained = false;
        QList<QByteArray> ownChannels;
    };
    struct Port
    {
        quint32 node = 0;
        bool output = false;
        QByteArray channel;
    };

    explicit Graph(pw_context *context) : core(context ? pw_context_connect(context, nullptr, 0) : nullptr)
    {
        if (!core)
            return;

        static const pw_core_events coreEvents = {
            .version = PW_VERSION_CORE_EVENTS,
            .done = [](void *graph, uint32_t, int seq) { static_cast<Graph *>(graph)->answered(seq); },
            .error = [](void *graph, uint32_t id, int, int result, const char *) { static_cast<Graph *>(graph)->failed(id, result); },
        };
        pw_core_add_listener(core, &coreListener, &coreEvents, this);

        static const pw_registry_events registryEvents = {
            .version = PW_VERSION_REGISTRY_EVENTS,
            .global = [](void *graph, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *properties) { static_cast<Graph *>(graph)->add(id, type, properties); },
            .global_remove = [](void *graph, uint32_t id) { static_cast<Graph *>(graph)->remove(id); },
        };
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry) {
            pw_core_disconnect(std::exchange(core, nullptr));
            return;
        }
        pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
    }

    ~Graph()
    {
        if (!core)
            return;
        // Disconnecting frees only the proxies that were destroyed first.
        for (auto &[id, client] : clients)
            pw_proxy_destroy(client.proxy);
        for (auto &[id, node] : nodes)
            pw_proxy_destroy(node.proxy);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
        pw_core_disconnect(core);
    }

    void sync() { pending = pw_core_sync(core, PW_ID_CORE, 0); }

    [[nodiscard]] bool isPlaying() const
    {
        return std::any_of(nodes.cbegin(), nodes.cend(), [](const auto &node) { return node.second.state == PW_NODE_STATE_RUNNING; });
    }

    pw_core *core;
    std::map<quint32, Client> clients;
    std::map<quint32, Node> nodes;
    std::map<quint32, Port> ports;
    bool sinkSeen = false;
    int answers = 0;
    std::function<void()> changed;
    std::function<void(int)> lost;

private:
    void answered(int seq)
    {
        if (seq == pending)
            answers++;
    }

    void failed(quint32 id, int result)
    {
        if (id == PW_ID_CORE && result == -EPIPE && lost)
            lost(result);
    }

    void tell()
    {
        if (changed)
            changed();
    }

    [[nodiscard]] pw_proxy *bind(quint32 id, const char *type, quint32 version)
    {
        auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(registry, id, type, version, 0));
        if (!proxy && lost)
            lost(-errno);
        return proxy;
    }

    void add(quint32 id, const char *type, const spa_dict *properties)
    {
        if (!properties)
            return;

        if (spa_streq(type, PW_TYPE_INTERFACE_Client)) {
            static const pw_client_events events = {
                .version = PW_VERSION_CLIENT_EVENTS,
                .info = [](void *graph, const pw_client_info *info) { static_cast<Graph *>(graph)->identify(info); },
            };
            pw_proxy *proxy = bind(id, type, PW_VERSION_CLIENT);
            if (!proxy)
                return;
            Client &client = clients[id];
            client.proxy = proxy;
            pw_proxy_add_object_listener(proxy, &client.listener, &events, this);
        } else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
            const char *mediaClass = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
            sinkSeen = sinkSeen || spa_streq(mediaClass, "Audio/Sink");
            if (!spa_streq(mediaClass, "Stream/Output/Audio"))
                return;

            static const pw_node_events events = {
                .version = PW_VERSION_NODE_EVENTS,
                .info = &Graph::follow,
                .param = &Graph::shape,
            };
            pw_proxy *proxy = bind(id, type, PW_VERSION_NODE);
            if (!proxy)
                return;
            Node &node = nodes[id];
            node.graph = this;
            node.proxy = proxy;
            if (const char *client = spa_dict_lookup(properties, PW_KEY_CLIENT_ID))
                node.client = QByteArray(client).toUInt();
            pw_proxy_add_object_listener(proxy, &node.listener, &events, &node);
            uint32_t wanted[] = { SPA_PARAM_Format };
            pw_node_subscribe_params(reinterpret_cast<pw_node *>(proxy), wanted, 1);
        } else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
            const char *channel = spa_dict_lookup(properties, PW_KEY_AUDIO_CHANNEL);
            if (!channel)
                return;
            ports[id] = Port{ QByteArray(spa_dict_lookup(properties, PW_KEY_NODE_ID)).toUInt(), spa_streq(spa_dict_lookup(properties, PW_KEY_PORT_DIRECTION), "out"), channel };
        } else {
            return;
        }

        tell();
    }

    void remove(quint32 id)
    {
        if (const auto client = clients.find(id); client != clients.end()) {
            pw_proxy_destroy(client->second.proxy);
            clients.erase(client);
        } else if (const auto node = nodes.find(id); node != nodes.end()) {
            pw_proxy_destroy(node->second.proxy);
            nodes.erase(node);
        } else if (ports.erase(id) == 0) {
            return;
        }

        tell();
    }

    static void follow(void *data, const pw_node_info *info)
    {
        auto *node = static_cast<Node *>(data);
        node->state = info->state;
        if (!info->props || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS))
            return;

        // The two ends of a loopback or a filter name the group that links them inside. What
        // such a stream plays is what other streams played first.
        node->chained = spa_dict_lookup(info->props, PW_KEY_NODE_LINK_GROUP) != nullptr;
        node->known = true;
        node->graph->tell();
    }

    // A node's ports are laid out as the sink it plays on. Its format is the program's own.
    static void shape(void *data, int, uint32_t, uint32_t, uint32_t, const spa_pod *format)
    {
        auto *node = static_cast<Node *>(data);
        spa_audio_info_raw raw = {};
        if (!format || spa_format_audio_raw_parse(format, &raw) < 0)
            return;

        node->ownChannels.clear();
        for (uint32_t i = 0; i < qMin<uint32_t>(raw.channels, std::size(raw.position)); i++)
            node->ownChannels.append(spa_debug_type_find_short_name(spa_type_audio_channel, raw.position[i]));
        node->graph->tell();
    }

    void identify(const pw_client_info *info)
    {
        const auto client = clients.find(info->id);
        if (client == clients.end() || !info->props)
            return;

        // PulseAudio's programs come through its server, which gives its own process as the
        // connection's and theirs as the application's. Only the connection's is the kernel's word.
        const bool throughPulse = spa_streq(spa_dict_lookup(info->props, PW_KEY_CLIENT_API), "pipewire-pulse");
        client->second.processId = QByteArray(spa_dict_lookup(info->props, throughPulse ? PW_KEY_APP_PROCESS_ID : PW_KEY_SEC_PID)).toUInt();
        client->second.name = QString::fromUtf8(spa_dict_lookup(info->props, PW_KEY_APP_NAME));
        if (client->second.name.isEmpty())
            client->second.name = QString::fromUtf8(spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY));
        client->second.known = true;
        client->second.inTree.reset();
        tell();
    }

    pw_registry *registry = nullptr;
    spa_hook coreListener = {};
    spa_hook registryListener = {};
    int pending = 0;
};

void consult(int roundTrips, const std::function<void(const Graph &)> &read)
{
    pw_main_loop *loop = initPipeWire() ? pw_main_loop_new(nullptr) : nullptr;
    pw_context *context = loop ? pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0) : nullptr;
    {
        Graph graph(context);
        if (graph.core) {
            pw_loop *events = pw_main_loop_get_loop(loop);
            QElapsedTimer timer;
            timer.start();
            pw_loop_enter(events);
            for (int round = 1; round <= roundTrips; round++) {
                graph.sync();
                for (int left = ANSWER_TIMEOUT_MS; graph.answers < round && left > 0; left = ANSWER_TIMEOUT_MS - static_cast<int>(timer.elapsed()))
                    pw_loop_iterate(events, left);
            }
            pw_loop_leave(events);
        }
        read(graph);
    }
    if (context)
        pw_context_destroy(context);
    if (loop)
        pw_main_loop_destroy(loop);
}

} // namespace

class LoopbackCapture::Worker
{
public:
    explicit Worker(LoopbackCapture *owner) : owner(owner) {}

    ~Worker()
    {
        if (!loop)
            return;
        pw_thread_loop_stop(loop);
        close();
        if (context)
            pw_context_destroy(context);
        pw_thread_loop_destroy(loop);
    }

    void request(const std::optional<Target> &target)
    {
        const std::lock_guard lock(mutex);
        wanted = target;
        requests++;
        if (target && !loop) {
            const bool installed = initPipeWire();
            loop = installed ? pw_thread_loop_new("acheron-loopback", nullptr) : nullptr;
            asked = loop ? pw_loop_add_event(pw_thread_loop_get_loop(loop), [](void *worker, uint64_t) { static_cast<Worker *>(worker)->serve(); }, this) : nullptr;
            const int unstarted = installed ? -errno : -ENOENT;
            const int started = asked ? pw_thread_loop_start(loop) : unstarted;
            if (started < 0) {
                if (loop)
                    pw_thread_loop_destroy(std::exchange(loop, nullptr));
                QMetaObject::invokeMethod(owner, [owner = owner, failure = failureText(QString::fromLocal8Bit(spa_strerror(started)))] { emit owner->failed(failure); }, Qt::QueuedConnection);
                return;
            }
        }
        if (loop)
            pw_loop_signal_event(pw_thread_loop_get_loop(loop), asked);
    }

private:
    void serve()
    {
        std::optional<Target> next;
        bool replaced = false;
        {
            const std::lock_guard lock(mutex);
            replaced = served != requests;
            served = requests;
            next = wanted;
        }
        const QString failure = std::exchange(broken, QString());
        if (!replaced && failure.isEmpty())
            return;

        close();
        if (!replaced) {
            qCWarning(LogVoice) << "Sound capture of process" << target.processId << "failed:" << failure;
            emit owner->failed(failure);
        } else if (next) {
            target = *next;
            open();
        }
    }

    void open()
    {
        pw_loop *events = pw_thread_loop_get_loop(loop);
        if (!context)
            context = pw_context_new(events, nullptr, 0);
        graph = std::make_unique<Graph>(context);
        pace = graph->core ? pw_loop_add_timer(events, [](void *worker, uint64_t) { static_cast<Worker *>(worker)->sendNext(); }, this) : nullptr;
        // Left unlinked by the session manager, which only links what asks to be connected.
        stream = pace ? pw_stream_new(graph->core, LoopbackCapture::tr("Shared sound").toUtf8().constData(), pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_NODE_AUTOCONNECT, "false", nullptr)) : nullptr;
        if (!stream) {
            fail(QString::fromLocal8Bit(strerror(errno)));
            return;
        }

        static const pw_stream_events streamEvents = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = [](void *worker, pw_stream_state, pw_stream_state state, const char *error) { static_cast<Worker *>(worker)->onState(state, error); },
            .process = [](void *worker) { static_cast<Worker *>(worker)->read(); },
        };
        pw_stream_add_listener(stream, &streamListener, &streamEvents, this);

        spa_audio_info_raw format = {};
        format.format = SPA_AUDIO_FORMAT_S16;
        format.rate = AUDIO_SAMPLE_RATE;
        format.channels = CAPTURED_CHANNELS;
        std::copy(std::begin(CAPTURED), std::end(CAPTURED), format.position);
        quint8 buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof(buffer));
        const spa_pod *parameters[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format) };
        const int connected = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, parameters, 1);
        if (connected < 0) {
            fail(QString::fromLocal8Bit(spa_strerror(connected)));
            return;
        }

        graph->changed = [this] { relink(); };
        graph->lost = [this](int result) { fail(QString::fromLocal8Bit(spa_strerror(result))); };
        keepPace();
    }

    void close()
    {
        if (!graph)
            return;
        if (pace)
            pw_loop_destroy_source(pw_thread_loop_get_loop(loop), std::exchange(pace, nullptr));
        if (stream)
            pw_stream_destroy(std::exchange(stream, nullptr));
        for (const auto &[ports, link] : links)
            pw_proxy_destroy(link);
        links.clear();
        graph.reset();
        unsent.clear();
        streaming = false;
    }

    // The capture is taken down from the loop, not from inside what PipeWire called to tell of it.
    void fail(const QString &detail)
    {
        broken = failureText(detail);
        pw_loop_signal_event(pw_thread_loop_get_loop(loop), asked);
    }

    void onState(pw_stream_state state, const char *error)
    {
        if (state == PW_STREAM_STATE_ERROR)
            fail(QString::fromUtf8(error));
        else
            streaming = state == PW_STREAM_STATE_STREAMING;
    }

    void keepPace()
    {
        timespec period = { 0, AUDIO_FRAME_DURATION_MS * 1000000 };
        pw_loop_update_timer(pw_thread_loop_get_loop(loop), pace, &period, &period, false);
    }

    // PipeWire hands over a whole cycle of the graph at once, which may be many frames long.
    // One goes out at once, and the rest a frame's length apart, to be gone before the next cycle.
    void read()
    {
        while (sendFrame()) {}

        while (pw_buffer *buffer = pw_stream_dequeue_buffer(stream)) {
            const spa_data &data = buffer->buffer->datas[0];
            if (data.data) {
                const quint32 offset = qMin(data.chunk->offset, data.maxsize);
                mixDown(reinterpret_cast<const qint16 *>(static_cast<const char *>(data.data) + offset), qMin(data.chunk->size, data.maxsize - offset) / (CAPTURED_CHANNELS * AUDIO_SAMPLE_BYTES));
            }
            pw_stream_queue_buffer(stream, buffer);
        }

        sendFrame();
        keepPace();
    }

    void mixDown(const qint16 *captured, quint32 sampleFrames)
    {
        // What PipeWire's own mixer gives the centre and surrounds, and the bass, when it makes
        // stereo of them (channelmix-ops.c).
        constexpr float BESIDE = 0.707106781f;
        constexpr float BASS = 0.353553391f;

        const auto had = unsent.size();
        unsent.resize(had + sampleFrames * AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES);
        auto *stereo = reinterpret_cast<qint16 *>(unsent.data() + had);
        for (quint32 i = 0; i < sampleFrames; i++, captured += CAPTURED_CHANNELS, stereo += AUDIO_CHANNELS) {
            const float both = BESIDE * captured[FC] + BASS * captured[LFE];
            stereo[0] = static_cast<qint16>(qBound(-32768.0f, captured[FL] + both + BESIDE * (captured[SL] + captured[RL]), 32767.0f));
            stereo[1] = static_cast<qint16>(qBound(-32768.0f, captured[FR] + both + BESIDE * (captured[SR] + captured[RR]), 32767.0f));
        }
    }

    bool sendFrame()
    {
        if (unsent.size() < AUDIO_FRAME_SIZE)
            return false;
        emit owner->frameCaptured(unsent.left(AUDIO_FRAME_SIZE));
        unsent.remove(0, AUDIO_FRAME_SIZE);
        return true;
    }

    // PipeWire only runs a node once something running is linked to it. Until then, while
    // others play, the silence is made here.
    void sendNext()
    {
        if (sendFrame() || streaming)
            return;

        if (!unsent.isEmpty()) {
            unsent.append(AUDIO_FRAME_SIZE - unsent.size(), '\0');
            sendFrame();
        } else if (graph->isPlaying()) {
            emit owner->frameCaptured(QByteArray(AUDIO_FRAME_SIZE, '\0'));
        }
    }

    [[nodiscard]] bool inScope(const Graph::Node &node)
    {
        if (!node.known || node.chained)
            return false;

        bool ofTree = false;
        if (node.client) {
            const auto client = graph->clients.find(*node.client);
            if (client == graph->clients.end() || !client->second.known)
                return false;
            if (!client->second.inTree)
                client->second.inTree = isInTree(client->second.processId, target.processId);
            ofTree = *client->second.inTree;
        }
        return ofTree == (target.scope == LoopbackCapture::Scope::ProcessTree);
    }

    // On a surround sink PipeWire fills the channels a program does not have from the ones it
    // has. Those are no part of what the program plays, and its front pair is unchanged by it.
    [[nodiscard]] static bool carries(const Graph::Node &node, const QByteArray &played, const QByteArray &captured)
    {
        if (captured == "FL" || captured == "FR")
            return played == captured || played == "MONO";
        return played == captured && node.ownChannels.contains(played);
    }

    void relink()
    {
        const quint32 self = pw_stream_get_node_id(stream);
        std::set<std::pair<quint32, quint32>> missing;
        for (const auto &[outputId, output] : graph->ports) {
            const auto node = graph->nodes.find(output.node);
            if (!output.output || node == graph->nodes.end() || !inScope(node->second))
                continue;
            for (const auto &[inputId, input] : graph->ports) {
                if (input.node == self && !input.output && carries(node->second, output.channel, input.channel))
                    missing.insert({ outputId, inputId });
            }
        }

        for (auto link = links.begin(); link != links.end();) {
            if (missing.erase(link->first) != 0) {
                ++link;
                continue;
            }
            pw_proxy_destroy(link->second);
            link = links.erase(link);
        }

        for (const auto &[output, input] : missing) {
            const QByteArray from = QByteArray::number(output);
            const QByteArray to = QByteArray::number(input);
            const spa_dict_item items[] = { { PW_KEY_LINK_OUTPUT_PORT, from.constData() }, { PW_KEY_LINK_INPUT_PORT, to.constData() } };
            const spa_dict properties = { 0, 2, items };
            auto *link = static_cast<pw_proxy *>(pw_core_create_object(graph->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &properties, 0));
            if (!link) {
                fail(QString::fromLocal8Bit(strerror(errno)));
                return;
            }
            links[{ output, input }] = link;
        }
    }

    LoopbackCapture *owner;

    std::mutex mutex;
    std::optional<Target> wanted;
    quint64 requests = 0;
    pw_thread_loop *loop = nullptr;
    spa_source *asked = nullptr;

    // The rest belongs to the loop's thread.
    quint64 served = 0;
    pw_context *context = nullptr;
    Target target;
    std::unique_ptr<Graph> graph;
    pw_stream *stream = nullptr;
    spa_hook streamListener = {};
    spa_source *pace = nullptr;
    std::map<std::pair<quint32, quint32>, pw_proxy *> links;
    QByteArray unsent;
    QString broken;
    bool streaming = false;
};

bool processLoopbackAvailable()
{
    static std::mutex mutex;
    static bool available = false;
    static QElapsedTimer asked;

    // No may be for now only: a session manager that is still starting, an output not yet plugged in.
    const std::lock_guard lock(mutex);
    if (available || (asked.isValid() && asked.elapsed() < ASKED_AGAIN_AFTER_MS))
        return available;

    // A PipeWire that only carries screens, beside a PulseAudio that plays, has no outputs.
    consult(1, [](const Graph &graph) { available = graph.sinkSeen; });
    asked.start();
    if (!available)
        qCInfo(LogVoice) << "There is no PipeWire that plays this machine's sound, so no program's sound can be captured";
    return available;
}

QList<PlayingProgram> playingPrograms()
{
    if (!processLoopbackAvailable())
        return {};

    // A program is told of in answer to binding it, which is asked as the registry lists it.
    QMap<quint32, QString> names;
    consult(2, [&names](const Graph &graph) {
        const auto self = static_cast<quint32>(getpid());
        for (const auto &[id, node] : graph.nodes) {
            const auto client = node.client ? graph.clients.find(*node.client) : graph.clients.end();
            if (!node.known || node.chained || client == graph.clients.end() || client->second.processId == 0 || isInTree(client->second.processId, self))
                continue;
            const auto named = names.find(client->second.processId);
            if (named == names.end() || client->second.name < *named)
                names.insert(client->second.processId, client->second.name);
        }
    });

    QList<PlayingProgram> programs;
    for (auto name = names.cbegin(); name != names.cend(); ++name)
        programs.append({ name.key(), name.value() });
    std::stable_sort(programs.begin(), programs.end(), [](const PlayingProgram &a, const PlayingProgram &b) { return a.name < b.name; });
    return programs;
}

LoopbackCapture::LoopbackCapture(QObject *parent) : QObject(parent), worker(std::make_unique<Worker>(this)) {}

LoopbackCapture::~LoopbackCapture() = default;

void LoopbackCapture::start(quint32 processId, Scope scope)
{
    worker->request(Target{ processId, scope });
}

void LoopbackCapture::stop()
{
    worker->request(std::nullopt);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron

#endif
