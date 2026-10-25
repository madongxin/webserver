#include "ControlResponseOutbox.h"

#include "Logging.h"
#include "OpsMetrics.h"
#include "ReplyStage.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

struct Item {
    uint64_t seq = 0;
    const char *kind = "other";
    std::string frame;
};

struct Box {
    std::mutex mu;
    std::deque<Item> q;
    size_t bytes = 0;
    bool inside = false;
    bool closed = false;
    bool blocked = false;
    /** 队头已经写进应用缓冲，还要等它离开后才能写下一条，避免新回包插到旧回包前面。 */
    bool held = false;
    ControlSendEnv env;
};

std::mutex g_map_mu;
std::unordered_map<uint64_t, std::shared_ptr<Box>> g_boxes;
ControlResponseOutbox::DiscardFn g_discard = nullptr;

std::atomic<uint64_t> g_overflow{0};
std::atomic<uint64_t> g_peak_frames{0};
std::atomic<uint64_t> g_peak_bytes{0};

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
    while (!box->q.empty()) {
        if (box->held) {
            box->inside = false;
            return saw ? first : 0;
        }
        Item item = box->q.front();
        auto try_send = box->env.try_send;
        auto drop_moves = box->env.drop_moves;
        lk.unlock();
        int code = try_send(item.frame);
        if (code == 0 && drop_moves) {
            drop_moves();
            if (g_discard)
                g_discard(conn_id);
            code = try_send(item.frame);
        }
        lk.lock();
        if (box->q.empty() || box->q.front().frame != item.frame) {
            continue;
        }
        if (code < 0) {
            box->closed = true;
            box->inside = false;
            box->blocked = true;
            return -1;
        }
        if (code == 0) {
            box->blocked = true;
            box->inside = false;
            return saw ? first : 0;
        }
        box->bytes -= box->q.front().frame.size();
        box->q.pop_front();
        box->blocked = false;
        box->held = code == 2;
        NotePeak(box->q.size(), box->bytes);
        NoteWritten(conn_id, item);
        if (!saw)
            first = 1;
        saw = true;
        if (box->held)
            break;
    }
    box->inside = false;
    if (box->q.empty() && !box->held)
        box->blocked = false;
    return first;
}

int ControlResponseOutbox::Submit(uint64_t conn_id, uint64_t seq, const char *kind,
                                  std::string frame, ControlSendEnv env) {
    if (conn_id == 0 || frame.empty())
        return -1;
    auto box = FindOrCreate(conn_id);
    std::unique_lock<std::mutex> lk(box->mu);
    if (box->closed)
        return -1;
    RememberEnv(*box, std::move(env));
    const bool queued_behind = !box->q.empty() || box->held || box->inside;
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
            box->held = code == 2;
            NoteWritten(conn_id, item);
            const int rc = box->held ? 2 : 1;
            if (!box->q.empty() && !box->held) {
                lk.unlock();
                Drain(conn_id);
            }
            return rc;
        }
        if (code < 0)
            return -1;
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
    if (box->closed)
        return -1;
    if (!Fits(*box, frame.size())) {
        box->closed = true;
        LOG_WARN << "control outbox overflow conn#" << conn_id << " depth=" << box->q.size()
                 << " bytes=" << box->bytes << " seq=" << seq;
        g_overflow.fetch_add(1, std::memory_order_relaxed);
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
    box->bytes += item.frame.size();
    box->q.push_back(std::move(item));
    NotePeak(box->q.size(), box->bytes);
    ReplyStage::Inc(box->q.back().kind, ReplyStage::ControlOutboxQueued);
    box->blocked = true;
    const bool start = !box->inside && !box->held && box->q.size() == 1;
    lk.unlock();
    if (start)
        Drain(conn_id);
    auto again = Find(conn_id);
    if (!again)
        return -1;
    std::lock_guard<std::mutex> guard(again->mu);
    if (again->closed)
        return -1;
    if (again->q.empty() && again->held)
        return 2;
    if (again->q.empty() && !again->held)
        return 1;
    return 0;
}

void ControlResponseOutbox::OnBufferDrained(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return;
    {
        std::lock_guard<std::mutex> lk(box->mu);
        if (box->inside || box->closed)
            return;
        box->held = false;
        if (box->q.empty())
            return;
    }
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
    box->q.clear();
    box->bytes = 0;
    box->closed = true;
    box->held = false;
    box->blocked = false;
}

bool ControlResponseOutbox::Holding(uint64_t conn_id) {
    auto box = Find(conn_id);
    if (!box)
        return false;
    std::lock_guard<std::mutex> lk(box->mu);
    if (box->closed)
        return false;
    return box->held;
}

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
    return !box->q.empty() || box->held;
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
    return std::string("# HELP gamemesh_control_outbox_peak_frames Peak RPC replies waiting on one connection.\n"
                       "# TYPE gamemesh_control_outbox_peak_frames gauge\n"
                       "gamemesh_control_outbox_peak_frames ") +
           std::to_string(g_peak_frames.load(std::memory_order_relaxed)) +
           "\n# HELP gamemesh_control_outbox_peak_bytes Peak RPC reply bytes waiting on one connection.\n"
           "# TYPE gamemesh_control_outbox_peak_bytes gauge\n"
           "gamemesh_control_outbox_peak_bytes " +
           std::to_string(g_peak_bytes.load(std::memory_order_relaxed)) +
           "\n# HELP gamemesh_control_outbox_overflow_total Connections closed because the RPC reply queue exceeded its cap.\n"
           "# TYPE gamemesh_control_outbox_overflow_total counter\n"
           "gamemesh_control_outbox_overflow_total " +
           std::to_string(g_overflow.load(std::memory_order_relaxed)) + "\n\n";
}
