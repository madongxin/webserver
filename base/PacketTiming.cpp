#include "PacketTiming.h"

#include "EventLoopMetrics.h"
#include "Logging.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

constexpr int kKindCount = 11;

const char *kKindNames[kKindCount] = {
    "heartbeat", "reconnect", "login",    "register", "enter_map", "query_lines",
    "move",      "snapshot",  "friend",   "push_ack", "other",
};

int KindIndex(const char *kind) {
    if (!kind || !kind[0])
        return kKindCount - 1;
    for (int i = 0; i < kKindCount; ++i) {
        if (std::strcmp(kind, kKindNames[i]) == 0)
            return i;
    }
    return kKindCount - 1;
}

enum Stage { kHandler = 0, kRpc = 1, kQueue = 2, kStageCount = 3 };

struct StageCounters {
    std::atomic<uint64_t> ms_sum{0};
    std::atomic<uint64_t> n{0};
    std::atomic<int> window_max{0};
    std::atomic<int> export_max{0};
};

StageCounters g_stage[kKindCount][kStageCount];
std::atomic<uint64_t> g_slow[kKindCount];

std::atomic<int> g_pending_window{0};
std::atomic<int> g_pending_export{0};

std::atomic<uint64_t> g_move_offer{0};
std::atomic<uint64_t> g_move_hold{0};
std::atomic<uint64_t> g_aoi_skip_retrans{0};
std::atomic<uint64_t> g_aoi_skip_unsent{0};
std::atomic<uint64_t> g_aoi_write_room{0};
std::atomic<uint64_t> g_aoi_write_starved{0};
uint64_t g_last_eth_tx = 0;
uint64_t g_last_retrans = 0;
bool g_bandwidth_seeded = false;
std::atomic<uint64_t> g_move_coalesce{0};
std::atomic<uint64_t> g_move_flush{0};
std::atomic<uint64_t> g_move_drop{0};
std::atomic<int> g_move_pending{0};

struct Worst {
    int ms = 0;
    int kind = kKindCount - 1;
    int stage = 0;
    uint64_t conn = 0;
    uint64_t player = 0;
    uint64_t seq = 0;
    int app = 0;
    int unsent = 0;
    int todo = 0;
};

std::atomic<int> g_worst_lock{0};
Worst g_worst;

void LockWorst() {
    int expected = 0;
    while (!g_worst_lock.compare_exchange_weak(expected, 1, std::memory_order_acquire))
        expected = 0;
}

void UnlockWorst() { g_worst_lock.store(0, std::memory_order_release); }

void StoreMax(std::atomic<int> *slot, int value) {
    int prev = slot->load(std::memory_order_relaxed);
    while (value > prev &&
           !slot->compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

bool AllowSlowLog() {
    static std::atomic<int64_t> sec{0};
    static std::atomic<int> n{0};
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t seen = sec.load(std::memory_order_relaxed);
    if (seen != now && sec.compare_exchange_strong(seen, now, std::memory_order_relaxed))
        n.store(0, std::memory_order_relaxed);
    return n.fetch_add(1, std::memory_order_relaxed) < 30;
}

const char *StageName(int stage) {
    if (stage == kRpc)
        return "rpc";
    if (stage == kQueue)
        return "reply_queue";
    return "handler";
}

}  // namespace

void NotePktStage(const char *kind, const char *stage, int64_t ms, uint64_t conn, uint64_t player,
                  uint64_t seq, int app_bytes, int unsent_bytes) {
    if (ms < 0)
        ms = 0;
    if (ms > 600000)
        ms = 600000;
    int si = kHandler;
    if (stage && std::strcmp(stage, "rpc") == 0)
        si = kRpc;
    else if (stage && std::strcmp(stage, "reply_queue") == 0)
        si = kQueue;
    const int ki = KindIndex(kind);
    const int ms_i = static_cast<int>(ms);
    auto &st = g_stage[ki][si];
    st.ms_sum.fetch_add(static_cast<uint64_t>(ms_i), std::memory_order_relaxed);
    st.n.fetch_add(1, std::memory_order_relaxed);
    StoreMax(&st.window_max, ms_i);
    StoreMax(&st.export_max, ms_i);
    if (ms_i >= 1000)
        g_slow[ki].fetch_add(1, std::memory_order_relaxed);

    if (ms_i >= 50) {
        LockWorst();
        if (ms_i >= g_worst.ms) {
            g_worst.ms = ms_i;
            g_worst.kind = ki;
            g_worst.stage = si;
            g_worst.conn = conn;
            g_worst.player = player;
            g_worst.seq = seq;
            g_worst.app = app_bytes;
            g_worst.unsent = unsent_bytes;
            g_worst.todo = EventLoopMetrics::LastTodoDepth();
        }
        UnlockWorst();
    }

    if (ms_i < 1000 || !AllowSlowLog())
        return;
    LOG_WARN << "slow pkt stage=" << StageName(si) << " kind=" << kKindNames[ki]
             << " conn=" << conn << " player=" << player << " seq=" << seq << " ms=" << ms_i
             << " app=" << app_bytes << " unsent=" << unsent_bytes
             << " todo=" << EventLoopMetrics::LastTodoDepth();
}

void NoteSendPending(int app_bytes, int unsent_bytes) {
    if (app_bytes < 0)
        app_bytes = 0;
    if (unsent_bytes < 0)
        unsent_bytes = 0;
    const int pending = app_bytes + unsent_bytes;
    StoreMax(&g_pending_window, pending);
    StoreMax(&g_pending_export, pending);
}

void NoteMoveOffer(bool scheduled_new) {
    g_move_offer.fetch_add(1, std::memory_order_relaxed);
    if (!scheduled_new)
        g_move_coalesce.fetch_add(1, std::memory_order_relaxed);
}

void NoteMoveFlush(bool sent, int entities) {
    if (sent)
        g_move_flush.fetch_add(1, std::memory_order_relaxed);
    else
        g_move_drop.fetch_add(1, std::memory_order_relaxed);
    if (entities > 0)
        g_move_pending.store(entities, std::memory_order_relaxed);
}

void NoteMoveHold(int entities) {
    g_move_hold.fetch_add(1, std::memory_order_relaxed);
    if (entities > 0)
        g_move_pending.store(entities, std::memory_order_relaxed);
}

void NoteAoiSkipRetrans() { g_aoi_skip_retrans.fetch_add(1, std::memory_order_relaxed); }
void NoteAoiSkipUnsent() { g_aoi_skip_unsent.fetch_add(1, std::memory_order_relaxed); }
void NoteAoiWriteRoom() { g_aoi_write_room.fetch_add(1, std::memory_order_relaxed); }
void NoteAoiWriteStarved() { g_aoi_write_starved.fetch_add(1, std::memory_order_relaxed); }

uint64_t ReadCounterFile(const char *path, const char *key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (key && line.find(key) == std::string::npos)
            continue;
        if (std::strncmp(line.c_str(), "Tcp:", 4) == 0 && std::strstr(line.c_str(), "RtoAlgorithm") == nullptr) {
            std::istringstream is(line);
            std::string tag;
            uint64_t v = 0;
            is >> tag;
            for (int i = 0; i < 13; ++i)
                is >> v;
            return v;
        }
        if (key && line.find(key) != std::string::npos) {
            std::istringstream is(line);
            std::string iface;
            uint64_t rx = 0, tx = 0;
            is >> iface >> rx;
            for (int i = 0; i < 7; ++i)
                is >> tx;
            is >> tx;
            return tx;
        }
    }
    return 0;
}

void NoteMovePendingEntities(int n) {
    if (n < 0)
        n = 0;
    g_move_pending.store(n, std::memory_order_relaxed);
}

void PacketTimingLogWindow() {
    const int pending = g_pending_window.exchange(0, std::memory_order_relaxed);
    g_pending_export.store(pending, std::memory_order_relaxed);
    const int todo = EventLoopMetrics::ExchangeRecentTodoMax();
    const int batch = EventLoopMetrics::ExchangeRecentBatchMax();
    const int batch_ms = EventLoopMetrics::ExchangeRecentBatchMsMax();
    const int loop_ms = static_cast<int>(EventLoopMetrics::LastTickSeconds() * 1000.0);

    Worst worst;
    LockWorst();
    worst = g_worst;
    g_worst = Worst();
    UnlockWorst();

    int handler_max[kKindCount];
    int queue_max[kKindCount];
    int rpc_max[kKindCount];
    for (int i = 0; i < kKindCount; ++i) {
        handler_max[i] = g_stage[i][kHandler].window_max.exchange(0, std::memory_order_relaxed);
        queue_max[i] = g_stage[i][kQueue].window_max.exchange(0, std::memory_order_relaxed);
        rpc_max[i] = g_stage[i][kRpc].window_max.exchange(0, std::memory_order_relaxed);
        g_stage[i][kHandler].export_max.store(handler_max[i], std::memory_order_relaxed);
        g_stage[i][kQueue].export_max.store(queue_max[i], std::memory_order_relaxed);
        g_stage[i][kRpc].export_max.store(rpc_max[i], std::memory_order_relaxed);
    }

    std::ostringstream kinds;
    for (int i = 0; i < kKindCount; ++i) {
        if (handler_max[i] < 200 && queue_max[i] < 200 && rpc_max[i] < 200)
            continue;
        kinds << " " << kKindNames[i] << ":h=" << handler_max[i] << " rpc=" << rpc_max[i]
              << " q=" << queue_max[i];
    }
    const uint64_t eth_tx = ReadCounterFile("/proc/net/dev", "eth0:");
    const uint64_t retrans = ReadCounterFile("/proc/net/snmp", nullptr);
    uint64_t eth_delta = 0;
    uint64_t retrans_delta = 0;
    if (g_bandwidth_seeded) {
        if (eth_tx >= g_last_eth_tx)
            eth_delta = eth_tx - g_last_eth_tx;
        if (retrans >= g_last_retrans)
            retrans_delta = retrans - g_last_retrans;
    }
    g_last_eth_tx = eth_tx;
    g_last_retrans = retrans;
    g_bandwidth_seeded = true;
    LOG_INFO << "sendq window=10s pending_max=" << pending << " todo_max=" << todo
             << " batch_max=" << batch << " batch_ms_max=" << batch_ms << " loop_ms=" << loop_ms
             << " move_offer=" << g_move_offer.load(std::memory_order_relaxed)
             << " move_coalesce=" << g_move_coalesce.load(std::memory_order_relaxed)
             << " move_flush=" << g_move_flush.load(std::memory_order_relaxed)
             << " move_drop=" << g_move_drop.load(std::memory_order_relaxed)
             << " move_pending=" << g_move_pending.load(std::memory_order_relaxed)
             << " aoi_skip_retrans=" << g_aoi_skip_retrans.load(std::memory_order_relaxed)
             << " aoi_skip_unsent=" << g_aoi_skip_unsent.load(std::memory_order_relaxed)
             << " aoi_write_room=" << g_aoi_write_room.load(std::memory_order_relaxed)
             << " aoi_write_starved=" << g_aoi_write_starved.load(std::memory_order_relaxed)
             << " eth0_tx_bytes_10s=" << eth_delta
             << " host_retrans_10s=" << retrans_delta;
    if (worst.ms >= 200) {
        LOG_INFO << "pkt window=10s slowest stage=" << StageName(worst.stage)
                 << " kind=" << kKindNames[worst.kind] << " ms=" << worst.ms
                 << " conn=" << worst.conn << " player=" << worst.player << " seq=" << worst.seq
                 << " app=" << worst.app << " unsent=" << worst.unsent << " todo=" << worst.todo
                 << kinds.str();
    } else if (!kinds.str().empty()) {
        LOG_INFO << "pkt window=10s" << kinds.str();
    }
}

std::string PacketTimingPrometheus() {
    std::ostringstream os;
    os << "# HELP gamemesh_send_pending_max_bytes Max app send buffer plus kernel unsent bytes in the last 10s window.\n"
          "# TYPE gamemesh_send_pending_max_bytes gauge\n"
          "gamemesh_send_pending_max_bytes "
       << g_pending_export.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_eventloop_todo_last Callbacks waiting on one event loop.\n"
          "# TYPE gamemesh_eventloop_todo_last gauge\n"
          "gamemesh_eventloop_todo_last " << EventLoopMetrics::LastTodoDepth() << "\n";
    os << "# HELP gamemesh_eventloop_batch_last Callbacks run in the last DoToDoList.\n"
          "# TYPE gamemesh_eventloop_batch_last gauge\n"
          "gamemesh_eventloop_batch_last " << EventLoopMetrics::LastBatchSize() << "\n";
    os << "# HELP gamemesh_eventloop_batch_ms_last Milliseconds spent in the last DoToDoList.\n"
          "# TYPE gamemesh_eventloop_batch_ms_last gauge\n"
          "gamemesh_eventloop_batch_ms_last "
       << static_cast<int>(EventLoopMetrics::LastBatchSeconds() * 1000.0) << "\n";
    os << "# HELP gamemesh_eventloop_tick_p99_ms Upper edge of the bucket containing the 99th percentile tick.\n"
          "# TYPE gamemesh_eventloop_tick_p99_ms gauge\n"
          "gamemesh_eventloop_tick_p99_ms " << EventLoopMetrics::TickP99Ms() << "\n";
    os << "# HELP gamemesh_eventloop_tick_max_ms Peak event loop iteration since process start, milliseconds.\n"
          "# TYPE gamemesh_eventloop_tick_max_ms gauge\n"
          "gamemesh_eventloop_tick_max_ms "
       << static_cast<int>(EventLoopMetrics::PeakTickSeconds() * 1000.0) << "\n";
    os << "# HELP gamemesh_move_offer_total Move AOI frames accepted into the per-connection coalesce slot.\n"
          "# TYPE gamemesh_move_offer_total counter\n"
          "gamemesh_move_offer_total " << g_move_offer.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_move_coalesce_total Move frames merged into an already scheduled flush.\n"
          "# TYPE gamemesh_move_coalesce_total counter\n"
          "gamemesh_move_coalesce_total " << g_move_coalesce.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_move_flush_total Coalesced move frames handed to the kernel write path.\n"
          "# TYPE gamemesh_move_flush_total counter\n"
          "gamemesh_move_flush_total " << g_move_flush.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_move_hold_total Latest move frame parked in the app buffer because kernel unsent exceeded 1KB.\n"
          "# TYPE gamemesh_move_hold_total counter\n"
          "gamemesh_move_hold_total " << g_move_hold.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_skip_retrans_total AOI position not written because this connection was retransmitting.\n"
          "# TYPE gamemesh_aoi_skip_retrans_total counter\n"
          "gamemesh_aoi_skip_retrans_total " << g_aoi_skip_retrans.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_skip_unsent_total AOI position not written because kernel unsent exceeded 16KB and the frame was younger than 1s.\n"
          "# TYPE gamemesh_aoi_skip_unsent_total counter\n"
          "gamemesh_aoi_skip_unsent_total " << g_aoi_skip_unsent.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_write_room_total AOI position written because kernel unsent was within 16KB.\n"
          "# TYPE gamemesh_aoi_write_room_total counter\n"
          "gamemesh_aoi_write_room_total " << g_aoi_write_room.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_write_starved_total AOI position written because it had been parked for at least 1s.\n"
          "# TYPE gamemesh_aoi_write_starved_total counter\n"
          "gamemesh_aoi_write_starved_total " << g_aoi_write_starved.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_move_drop_total Coalesced move frames not queued because non-move app bytes already exceeded 64KB.\n"
          "# TYPE gamemesh_move_drop_total counter\n"
          "gamemesh_move_drop_total " << g_move_drop.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_move_pending_entities Entities held in the latest move coalesce slot.\n"
          "# TYPE gamemesh_move_pending_entities gauge\n"
          "gamemesh_move_pending_entities " << g_move_pending.load(std::memory_order_relaxed) << "\n";

    os << "# HELP gamemesh_pkt_stage_ms_sum Sum of packet stage latency in milliseconds.\n"
          "# TYPE gamemesh_pkt_stage_ms_sum counter\n";
    os << "# HELP gamemesh_pkt_stage_count Packet stage samples.\n"
          "# TYPE gamemesh_pkt_stage_count counter\n";
    os << "# HELP gamemesh_pkt_stage_max_ms Max stage latency in the last 10s window.\n"
          "# TYPE gamemesh_pkt_stage_max_ms gauge\n";
    os << "# HELP gamemesh_pkt_slow_total Stages at or above 1000ms.\n"
          "# TYPE gamemesh_pkt_slow_total counter\n";
    const char *stage_name[kStageCount] = {"handler", "rpc", "reply_queue"};
    for (int k = 0; k < kKindCount; ++k) {
        for (int s = 0; s < kStageCount; ++s) {
            os << "gamemesh_pkt_stage_ms_sum{kind=\"" << kKindNames[k] << "\",stage=\""
               << stage_name[s] << "\"} " << g_stage[k][s].ms_sum.load(std::memory_order_relaxed)
               << "\n";
            os << "gamemesh_pkt_stage_count{kind=\"" << kKindNames[k] << "\",stage=\""
               << stage_name[s] << "\"} " << g_stage[k][s].n.load(std::memory_order_relaxed)
               << "\n";
            os << "gamemesh_pkt_stage_max_ms{kind=\"" << kKindNames[k] << "\",stage=\""
               << stage_name[s] << "\"} "
               << g_stage[k][s].export_max.load(std::memory_order_relaxed) << "\n";
        }
        os << "gamemesh_pkt_slow_total{kind=\"" << kKindNames[k] << "\"} "
           << g_slow[k].load(std::memory_order_relaxed) << "\n";
    }
    os << "\n";
    return os.str();
}
