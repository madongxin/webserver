#include "ControlResponseOutbox.h"

#include "Logging.h"
#include "OpsMetrics.h"
#include "ReplyStage.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Item {
    uint64_t seq = 0;
    const char *kind = "other";
    std::string frame;
    int64_t queued_ms = 0;
};

struct Box {
    std::mutex mu;
    std::deque<Item> q;
    size_t bytes = 0;
    bool inside = false;
    bool closed = false;
    bool blocked = false;
    /** 已经预约了一次 50ms 重试。同一条连接最多一个。 */
    bool retry_armed = false;
    ControlSendEnv env;
};

std::mutex g_map_mu;
std::unordered_map<uint64_t, std::shared_ptr<Box>> g_boxes;
ControlResponseOutbox::DiscardFn g_discard = nullptr;

std::atomic<uint64_t> g_overflow{0};
std::atomic<uint64_t> g_peak_frames{0};
std::atomic<uint64_t> g_peak_bytes{0};
std::atomic<uint64_t> g_drain_runs{0};
std::atomic<uint64_t> g_drain_stalls{0};
std::atomic<uint64_t> g_wakeup_missed{0};
std::atomic<uint64_t> g_write_errors{0};

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void NoteWritten(uint64_t conn_id, const Item &item);
std::shared_ptr<Box> Find(uint64_t conn_id);

void CountClosed(const char *kind) {
    ReplyStage::Inc(kind && kind[0] ? kind : "other", ReplyStage::ConnectionClosedBeforeReply);
}

void AccountWritten(Box *box, uint64_t conn_id, const Item &item, int code) {
    (void)box;
    NoteWritten(conn_id, item);
    // 直接写出和进入应用缓冲都表示这一帧已经按序进入发送流。
    if (code > 0)
        ReplyStage::Inc(item.kind, ReplyStage::WriteComplete);
}

void ArmWatchdog(uint64_t conn_id, const std::shared_ptr<Box> &box) {
    std::function<void(std::function<void()>)> arm;
    {
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->retry_armed || box->closed || box->q.empty() || !box->env.arm_retry)
            return;
        box->retry_armed = true;
        arm = box->env.arm_retry;
    }
    // arm_retry 若在调用栈里直接执行回调，不再嵌套预约，避免空转。
    thread_local int depth = 0;
    if (depth > 0) {
        std::lock_guard<std::mutex> lk(box->mu);
        box->retry_armed = false;
        return;
    }
    ++depth;
    arm([conn_id]() {
        if (auto live = Find(conn_id)) {
            std::lock_guard<std::mutex> lk(live->mu);
            live->retry_armed = false;
        }
        ControlResponseOutbox::Instance().OnBufferDrained(conn_id);
    });
    --depth;
}

std::shared_ptr<Box> FindOrCreate(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_map_mu);
    auto &slot = g_boxes[conn_id];
    if (!slot)
        slot = std::make_shared<Box>();
    return slot;
}

std::shared_ptr<Box> Find(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_map_mu);
    auto it = g_boxes.find(conn_id);
    if (it == g_boxes.end())
        return nullptr;
    return it->second;
}

void NotePeak(size_t frames, size_t bytes) {
    uint64_t prev_f = g_peak_frames.load(std::memory_order_relaxed);
    while (frames > prev_f &&
           !g_peak_frames.compare_exchange_weak(prev_f, frames, std::memory_order_relaxed)) {
    }
    uint64_t prev_b = g_peak_bytes.load(std::memory_order_relaxed);
    while (bytes > prev_b &&
           !g_peak_bytes.compare_exchange_weak(prev_b, bytes, std::memory_order_relaxed)) {
    }
}

void NoteWritten(uint64_t conn_id, const Item &item) {
    OpsMetrics::Instance().IncReplyQueueAccepted();
    OpsMetrics::Instance().AddGatewayTxBytes(item.frame.size());
    OpsMetrics::Instance().AddGatewayTxFrames(1);
    ReplyStage::Inc(item.kind, ReplyStage::SocketWriteAccepted);
    ReplyStage::NoteWritten(conn_id, item.seq, item.kind);
    if (item.kind && std::strcmp(item.kind, "heartbeat") == 0)
        OpsMetrics::Instance().IncHeartbeatOk();
}

bool Fits(const Box &box, size_t frame_bytes) {
    return box.q.size() < ControlResponseOutbox::kMaxFrames &&
           box.bytes + frame_bytes <= ControlResponseOutbox::kMaxBytes;
}

void RememberEnv(Box &box, ControlSendEnv env) {
    if (env.try_send)
        box.env.try_send = std::move(env.try_send);
    if (env.drop_moves)
        box.env.drop_moves = std::move(env.drop_moves);
    if (env.close_conn)
        box.env.close_conn = std::move(env.close_conn);
    if (env.arm_retry)
        box.env.arm_retry = std::move(env.arm_retry);
}

size_t DropMoves(uint64_t conn_id, Box &box) {
    size_t n = 0;
    if (box.env.drop_moves)
        n += box.env.drop_moves();
    if (g_discard)
        g_discard(conn_id);
    return n;
}

}  // namespace

ControlResponseOutbox &ControlResponseOutbox::Instance() {
    static ControlResponseOutbox hub;
    return hub;
}

void ControlResponseOutbox::SetPendingMoveDiscard(DiscardFn fn) { g_discard = fn; }

int ControlResponseOutbox::Drain(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return -1;
    std::unique_lock<std::mutex> lk(box->mu);
    if (box->closed)
        return -1;
    if (box->inside)
        return 1;
    if (!box->env.try_send)
        return -1;
    box->inside = true;
    int first = 1;
    bool saw = false;
    for (;;) {
        while (!box->q.empty()) {
            Item item = box->q.front();
            auto try_send = box->env.try_send;
            auto drop_moves = box->env.drop_moves;
            lk.unlock();
            g_drain_runs.fetch_add(1, std::memory_order_relaxed);
            int code = try_send(item.frame);
            if (code == 0 && drop_moves) {
                drop_moves();
                if (g_discard)
                    g_discard(conn_id);
                g_drain_runs.fetch_add(1, std::memory_order_relaxed);
                code = try_send(item.frame);
            }
            lk.lock();
            if (box->q.empty() || box->q.front().frame != item.frame) {
                continue;
            }
            if (code < 0) {
                g_write_errors.fetch_add(1, std::memory_order_relaxed);
                box->closed = true;
                box->inside = false;
                box->blocked = true;
                return -1;
            }
            if (code == 0) {
                g_drain_stalls.fetch_add(1, std::memory_order_relaxed);
                box->blocked = true;
                box->inside = false;
                lk.unlock();
                ArmWatchdog(conn_id, box);
                return saw ? first : 0;
            }
            box->bytes -= box->q.front().frame.size();
            box->q.pop_front();
            box->blocked = false;
            NotePeak(box->q.size(), box->bytes);
            AccountWritten(box.get(), conn_id, item, code);
            if (!saw)
                first = code;
            saw = true;
        }
        box->inside = false;
        if (!box->q.empty() && !box->closed && box->env.try_send) {
            g_wakeup_missed.fetch_add(1, std::memory_order_relaxed);
            ReplyStage::Inc(box->q.front().kind, ReplyStage::OutboxWakeupMissed);
            box->inside = true;
            continue;
        }
        break;
    }
    if (box->q.empty())
        box->blocked = false;
    return saw ? first : 1;
}

int ControlResponseOutbox::Submit(uint64_t conn_id, uint64_t seq, const char *kind,
                                  std::string frame, ControlSendEnv env) {
    if (conn_id == 0 || frame.empty())
        return -1;
    auto box = FindOrCreate(conn_id);
    std::unique_lock<std::mutex> lk(box->mu);
    if (box->closed) {
        CountClosed(kind);
        return -1;
    }
    RememberEnv(*box, std::move(env));
    const bool queued_behind = !box->q.empty() || box->inside;
    if (!queued_behind && box->env.try_send) {
        Item item;
        item.seq = seq;
        item.kind = kind && kind[0] ? kind : "other";
        item.frame = frame;
        auto try_send = box->env.try_send;
        auto drop_moves = box->env.drop_moves;
        box->inside = true;
        lk.unlock();
        int code = try_send(item.frame);
        if (code == 0 && drop_moves) {
            drop_moves();
            if (g_discard)
                g_discard(conn_id);
            code = try_send(item.frame);
        }
        lk.lock();
        box->inside = false;
        if (code > 0) {
            AccountWritten(box.get(), conn_id, item, code);
            const int rc = code;
            const bool more = !box->q.empty();
            lk.unlock();
            if (more)
                Drain(conn_id);
            return rc;
        }
        if (code < 0) {
            g_write_errors.fetch_add(1, std::memory_order_relaxed);
            CountClosed(item.kind);
            return -1;
        }
        frame = std::move(item.frame);
        kind = item.kind;
        seq = item.seq;
    }
    if (!Fits(*box, frame.size())) {
        lk.unlock();
        DropMoves(conn_id, *box);
        Drain(conn_id);
        lk.lock();
    }
    if (box->closed) {
        CountClosed(kind);
        return -1;
    }
    if (!Fits(*box, frame.size())) {
        box->closed = true;
        LOG_WARN << "control outbox overflow conn#" << conn_id << " depth=" << box->q.size()
                 << " bytes=" << box->bytes << " seq=" << seq;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        CountClosed(kind);
        auto close_conn = box->env.close_conn;
        lk.unlock();
        if (close_conn)
            close_conn();
        return -1;
    }
    Item item;
    item.seq = seq;
    item.kind = kind && kind[0] ? kind : "other";
    item.frame = std::move(frame);
    item.queued_ms = SteadyMs();
    box->bytes += item.frame.size();
    box->q.push_back(std::move(item));
    NotePeak(box->q.size(), box->bytes);
    ReplyStage::Inc(box->q.back().kind, ReplyStage::ControlOutboxQueued);
    box->blocked = true;
    const bool start = !box->inside && box->q.size() == 1;
    lk.unlock();
    if (start)
        Drain(conn_id);
    auto again = Find(conn_id);
    if (!again)
        return -1;
    std::lock_guard<std::mutex> guard(again->mu);
    if (again->closed)
        return -1;
    if (again->q.empty())
        return 1;
    return 0;
}

void ControlResponseOutbox::OnBufferDrained(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return;
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->inside || box->closed)
            return;
        more = !box->q.empty();
    }
    if (more)
        Drain(conn_id);
}

void ControlResponseOutbox::Forget(uint64_t conn_id) {
    std::shared_ptr<Box> box;
    {
        std::lock_guard<std::mutex> lk(g_map_mu);
        auto it = g_boxes.find(conn_id);
        if (it == g_boxes.end())
            return;
        box = it->second;
        g_boxes.erase(it);
    }
    std::lock_guard<std::mutex> lk(box->mu);
    for (const auto &item : box->q)
        CountClosed(item.kind);
    box->q.clear();
    box->bytes = 0;
    box->closed = true;
    box->blocked = false;
    box->retry_armed = false;
}

bool ControlResponseOutbox::Holding(uint64_t) { return false; }

size_t ControlResponseOutbox::Depth(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return 0;
    std::lock_guard<std::mutex> lk(box->mu);
    if (box->closed)
        return 0;
    return box->q.size();
}

bool ControlResponseOutbox::BlocksMove(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return false;
    std::lock_guard<std::mutex> lk(box->mu);
    if (box->closed)
        return false;
    return !box->q.empty();
}

namespace {
struct InstallControlOutboxPrometheus {
    InstallControlOutboxPrometheus() {
        OpsMetrics::SetControlOutboxPrometheus(&ControlResponseOutbox::Prometheus);
    }
};
InstallControlOutboxPrometheus g_install_control_outbox_prometheus;
}  // namespace

std::string ControlResponseOutbox::Prometheus() {
    uint64_t frames = 0;
    uint64_t bytes = 0;
    uint64_t held_conns = 0;
    int64_t oldest = 0;
    const int64_t now = SteadyMs();
    std::vector<std::shared_ptr<Box>> boxes;
    {
        std::lock_guard<std::mutex> lk(g_map_mu);
        boxes.reserve(g_boxes.size());
        for (const auto &kv : g_boxes)
            boxes.push_back(kv.second);
    }
    for (const auto &box : boxes) {
        if (!box)
            continue;
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->closed)
            continue;
        frames += box->q.size();
        bytes += box->bytes;
        if (!box->q.empty()) {
            ++held_conns;
            const int64_t since = box->q.front().queued_ms;
            if (since > 0 && now >= since) {
                const int64_t age = now - since;
                if (age > oldest)
                    oldest = age;
            }
        }
    }
    std::ostringstream os;
    os << "# HELP gamemesh_control_outbox_current_frames RPC replies still queued, not yet in the socket buffer.\n"
          "# TYPE gamemesh_control_outbox_current_frames gauge\n"
          "gamemesh_control_outbox_current_frames "
       << frames
       << "\n# HELP gamemesh_control_outbox_current_bytes Bytes of RPC replies still queued.\n"
          "# TYPE gamemesh_control_outbox_current_bytes gauge\n"
          "gamemesh_control_outbox_current_bytes "
       << bytes
       << "\n# HELP gamemesh_control_outbox_peak_frames Peak RPC replies waiting on one connection.\n"
          "# TYPE gamemesh_control_outbox_peak_frames gauge\n"
          "gamemesh_control_outbox_peak_frames "
       << g_peak_frames.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_peak_bytes Peak RPC reply bytes waiting on one connection.\n"
          "# TYPE gamemesh_control_outbox_peak_bytes gauge\n"
          "gamemesh_control_outbox_peak_bytes "
       << g_peak_bytes.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_held_connections Connections with an RPC reply not yet in the send stream.\n"
          "# TYPE gamemesh_control_outbox_held_connections gauge\n"
          "gamemesh_control_outbox_held_connections "
       << held_conns
       << "\n# HELP gamemesh_control_outbox_oldest_held_ms Age of the oldest RPC reply not yet in the send stream.\n"
          "# TYPE gamemesh_control_outbox_oldest_held_ms gauge\n"
          "gamemesh_control_outbox_oldest_held_ms "
       << oldest
       << "\n# HELP gamemesh_control_outbox_drain_runs_total Attempts to write a queued RPC reply.\n"
          "# TYPE gamemesh_control_outbox_drain_runs_total counter\n"
          "gamemesh_control_outbox_drain_runs_total "
       << g_drain_runs.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_drain_stalls_total Drains that stopped because the socket accepted nothing.\n"
          "# TYPE gamemesh_control_outbox_drain_stalls_total counter\n"
          "gamemesh_control_outbox_drain_stalls_total "
       << g_drain_stalls.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_wakeup_missed_total Drains that finished while a reply was queued behind them.\n"
          "# TYPE gamemesh_control_outbox_wakeup_missed_total counter\n"
          "gamemesh_control_outbox_wakeup_missed_total "
       << g_wakeup_missed.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_write_errors_total RPC reply writes that found the connection gone.\n"
          "# TYPE gamemesh_control_outbox_write_errors_total counter\n"
          "gamemesh_control_outbox_write_errors_total "
       << g_write_errors.load(std::memory_order_relaxed)
       << "\n# HELP gamemesh_control_outbox_overflow_total Connections closed because the RPC reply queue exceeded its cap.\n"
          "# TYPE gamemesh_control_outbox_overflow_total counter\n"
          "gamemesh_control_outbox_overflow_total "
       << g_overflow.load(std::memory_order_relaxed) << "\n\n";
    return os.str();
}
