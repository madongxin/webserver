#include "ReplyStage.h"

#include "OpsMetrics.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace {

constexpr int kKindCount = 12;
constexpr int kBucketCount = 11;

const char *kKinds[kKindCount] = {
    "heartbeat", "reconnect", "login",  "register", "enter_map", "query_lines",
    "move",      "snapshot",  "friend", "push_ack", "hello",     "other",
};

const char *kStages[ReplyStage::StageCount] = {
    "request_received",      "logic_dispatch_started", "logic_response_ready",
    "event_loop_enqueued",   "control_outbox_queued",  "socket_write_accepted",
    "response_soft_drop",    "gateway_callback_received", "write_complete",
    "connection_closed_before_reply", "event_loop_enqueue_failed", "outbox_wakeup_missed",
};

const int kBucketMs[kBucketCount] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 1000000000};

int KindIndex(const char *kind) {
    if (!kind || !kind[0])
        return kKindCount - 1;
    for (int i = 0; i < kKindCount; ++i) {
        if (std::strcmp(kind, kKinds[i]) == 0)
            return i;
    }
    return kKindCount - 1;
}

std::atomic<uint64_t> g_stage[kKindCount][ReplyStage::StageCount];
std::atomic<uint64_t> g_bucket[kKindCount][kBucketCount];
std::atomic<uint64_t> g_latency_sum[kKindCount];
std::atomic<uint64_t> g_latency_count[kKindCount];
/** 还没有进入终态的请求。第二次终态不再减，用来让重复的 write_complete 把守恒差值打成非零。 */
std::atomic<int64_t> g_inflight{0};
std::atomic<int64_t> g_last_complete_ms{0};
std::atomic<int64_t> g_inflight_since_ms{0};

bool TerminalStage(ReplyStage::Stage stage) {
    return stage == ReplyStage::WriteComplete || stage == ReplyStage::ConnectionClosedBeforeReply ||
           stage == ReplyStage::ResponseSoftDrop;
}

struct Stamp {
    int64_t ms = 0;
    int kind = 0;
};

constexpr int kShards = 16;
std::mutex g_stamp_mu[kShards];
std::unordered_map<uint64_t, Stamp> g_stamps[kShards];

uint64_t StampKey(uint64_t conn_id, uint64_t seq) {
    return (conn_id * 1315423911u) ^ (seq + 0x9e3779b97f4a7c15ull);
}

int ShardOf(uint64_t key) { return static_cast<int>(key % kShards); }

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AddLatency(int kind, int ms) {
    if (ms < 0)
        ms = 0;
    if (ms > 600000)
        ms = 600000;
    int bucket = kBucketCount - 1;
    for (int i = 0; i < kBucketCount - 1; ++i) {
        if (ms <= kBucketMs[i]) {
            bucket = i;
            break;
        }
    }
    g_bucket[kind][bucket].fetch_add(1, std::memory_order_relaxed);
    g_latency_sum[kind].fetch_add(static_cast<uint64_t>(ms), std::memory_order_relaxed);
    g_latency_count[kind].fetch_add(1, std::memory_order_relaxed);
}

int QuantileMs(int kind, int pct) {
    const uint64_t count = g_latency_count[kind].load(std::memory_order_relaxed);
    if (count == 0 || pct <= 0)
        return 0;
    uint64_t rank = (count * static_cast<uint64_t>(pct) + 99) / 100;
    if (rank < 1)
        rank = 1;
    if (rank > count)
        rank = count;
    uint64_t seen = 0;
    for (int i = 0; i < kBucketCount; ++i) {
        seen += g_bucket[kind][i].load(std::memory_order_relaxed);
        if (seen >= rank)
            return kBucketMs[i] >= 1000000000 ? 5001 : kBucketMs[i];
    }
    return 5001;
}

}  // namespace

void ReplyStage::Inc(const char *kind, Stage stage) {
    if (stage < 0 || stage >= StageCount)
        return;
    g_stage[KindIndex(kind)][stage].fetch_add(1, std::memory_order_relaxed);
    if (stage == RequestReceived) {
        const int64_t prev = g_inflight.fetch_add(1, std::memory_order_relaxed);
        if (prev <= 0)
            g_inflight_since_ms.store(NowMs(), std::memory_order_relaxed);
        return;
    }
    if (!TerminalStage(stage))
        return;
    int64_t cur = g_inflight.load(std::memory_order_relaxed);
    while (cur > 0 &&
           !g_inflight.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) {
    }
    if (stage == WriteComplete)
        g_last_complete_ms.store(NowMs(), std::memory_order_relaxed);
}

void ReplyStage::NoteRequest(uint64_t conn_id, uint64_t seq, const char *kind) {
    if (conn_id == 0 || seq == 0)
        return;
    const uint64_t key = StampKey(conn_id, seq);
    const int shard = ShardOf(key);
    std::lock_guard<std::mutex> lk(g_stamp_mu[shard]);
    auto &table = g_stamps[shard];
    if (table.size() > 8192)
        table.clear();
    Stamp stamp;
    stamp.ms = NowMs();
    stamp.kind = KindIndex(kind);
    table[key] = stamp;
}

void ReplyStage::NoteWritten(uint64_t conn_id, uint64_t seq, const char *kind) {
    if (conn_id == 0 || seq == 0)
        return;
    const uint64_t key = StampKey(conn_id, seq);
    const int shard = ShardOf(key);
    int64_t born = 0;
    int stored_kind = KindIndex(kind);
    {
        std::lock_guard<std::mutex> lk(g_stamp_mu[shard]);
        auto it = g_stamps[shard].find(key);
        if (it == g_stamps[shard].end())
            return;
        born = it->second.ms;
        stored_kind = it->second.kind;
        g_stamps[shard].erase(it);
    }
    int ms = static_cast<int>(NowMs() - born);
    AddLatency(stored_kind, ms);
}

std::string ReplyStage::Prometheus() {
    std::ostringstream os;
    os << "# HELP gamemesh_reply_stage_total RPC reply pipeline counters.\n"
          "# TYPE gamemesh_reply_stage_total counter\n";
    for (int k = 0; k < kKindCount; ++k) {
        for (int s = 0; s < StageCount; ++s) {
            os << "gamemesh_reply_stage_total{kind=\"" << kKinds[k] << "\",stage=\"" << kStages[s]
               << "\"} " << g_stage[k][s].load(std::memory_order_relaxed) << "\n";
        }
    }
    os << "\n# HELP gamemesh_response_soft_drop_total Replies dropped while the TCP connection stayed up.\n"
          "# TYPE gamemesh_response_soft_drop_total counter\n";
    uint64_t soft = 0;
    for (int k = 0; k < kKindCount; ++k)
        soft += g_stage[k][ResponseSoftDrop].load(std::memory_order_relaxed);
    os << "gamemesh_response_soft_drop_total " << soft << "\n\n";

    os << "# HELP gamemesh_reply_latency_ms_bucket Request receive to socket-buffer accept.\n"
          "# TYPE gamemesh_reply_latency_ms_bucket histogram\n";
    os << "# HELP gamemesh_reply_latency_ms_sum Sum of request-to-socket-accept milliseconds.\n"
          "# TYPE gamemesh_reply_latency_ms_sum counter\n";
    os << "# HELP gamemesh_reply_latency_ms_count Replies with a request timestamp.\n"
          "# TYPE gamemesh_reply_latency_ms_count counter\n";
    os << "# HELP gamemesh_reply_latency_p50_ms Upper edge of the bucket containing the 50th percentile.\n"
          "# TYPE gamemesh_reply_latency_p50_ms gauge\n";
    os << "# HELP gamemesh_reply_latency_p95_ms Upper edge of the bucket containing the 95th percentile.\n"
          "# TYPE gamemesh_reply_latency_p95_ms gauge\n";
    os << "# HELP gamemesh_reply_latency_p99_ms Upper edge of the bucket containing the 99th percentile.\n"
          "# TYPE gamemesh_reply_latency_p99_ms gauge\n";
    for (int k = 0; k < kKindCount; ++k) {
        uint64_t cum = 0;
        for (int b = 0; b < kBucketCount; ++b) {
            cum += g_bucket[k][b].load(std::memory_order_relaxed);
            const char *le = b == kBucketCount - 1 ? "+Inf" : nullptr;
            if (le) {
                os << "gamemesh_reply_latency_ms_bucket{kind=\"" << kKinds[k] << "\",le=\"+Inf\"} "
                   << cum << "\n";
            } else {
                os << "gamemesh_reply_latency_ms_bucket{kind=\"" << kKinds[k] << "\",le=\""
                   << kBucketMs[b] << "\"} " << cum << "\n";
            }
        }
        os << "gamemesh_reply_latency_ms_sum{kind=\"" << kKinds[k] << "\"} "
           << g_latency_sum[k].load(std::memory_order_relaxed) << "\n";
        os << "gamemesh_reply_latency_ms_count{kind=\"" << kKinds[k] << "\"} "
           << g_latency_count[k].load(std::memory_order_relaxed) << "\n";
        os << "gamemesh_reply_latency_p50_ms{kind=\"" << kKinds[k] << "\"} " << QuantileMs(k, 50)
           << "\n";
        os << "gamemesh_reply_latency_p95_ms{kind=\"" << kKinds[k] << "\"} " << QuantileMs(k, 95)
           << "\n";
        os << "gamemesh_reply_latency_p99_ms{kind=\"" << kKinds[k] << "\"} " << QuantileMs(k, 99)
           << "\n";
    }
    uint64_t received_n = 0, completed_n = 0, closed_n = 0, soft_n = 0;
    for (int k = 0; k < kKindCount; ++k) {
        received_n += g_stage[k][RequestReceived].load(std::memory_order_relaxed);
        completed_n += g_stage[k][WriteComplete].load(std::memory_order_relaxed);
        closed_n += g_stage[k][ConnectionClosedBeforeReply].load(std::memory_order_relaxed);
        soft_n += g_stage[k][ResponseSoftDrop].load(std::memory_order_relaxed);
    }
    const int64_t inflight = g_inflight.load(std::memory_order_relaxed);
    const int64_t residual = static_cast<int64_t>(received_n) - static_cast<int64_t>(completed_n) -
                             static_cast<int64_t>(closed_n) - static_cast<int64_t>(soft_n) - inflight;
    int64_t idle_ms = 0;
    if (inflight > 0) {
        const int64_t now = NowMs();
        int64_t since = g_last_complete_ms.load(std::memory_order_relaxed);
        if (since <= 0)
            since = g_inflight_since_ms.load(std::memory_order_relaxed);
        if (since > 0 && now >= since)
            idle_ms = now - since;
    }
    os << "\n# HELP gamemesh_reply_inflight Requests not yet write_complete, closed, or soft-dropped.\n"
          "# TYPE gamemesh_reply_inflight gauge\n"
          "gamemesh_reply_inflight "
       << inflight
       << "\n# HELP gamemesh_reply_conservation_residual request_received minus write_complete, "
          "connection_closed_before_reply, response_soft_drop, and inflight.\n"
          "# TYPE gamemesh_reply_conservation_residual gauge\n"
          "gamemesh_reply_conservation_residual "
       << residual
       << "\n# HELP gamemesh_reply_write_complete_idle_ms Milliseconds since the last write_complete "
          "while a reply is still in flight. Zero when nothing is in flight.\n"
          "# TYPE gamemesh_reply_write_complete_idle_ms gauge\n"
          "gamemesh_reply_write_complete_idle_ms "
       << idle_ms << "\n\n";
    return os.str();
}

namespace {
struct InstallReplyStagePrometheus {
    InstallReplyStagePrometheus() {
        OpsMetrics::SetReplyStagePrometheus(&ReplyStage::Prometheus);
    }
};
InstallReplyStagePrometheus g_install_reply_stage_prometheus;
}  // namespace
