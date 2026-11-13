#include "AoiEvidence.h"
#include "ServerStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#ifndef SIOCOUTQNSD
#define SIOCOUTQNSD 0x894B
#endif

namespace {

constexpr int kCellCap = 9;
constexpr int kFailHistCap = 64;
constexpr uint32_t kTraceCapDefault = 4096;
constexpr uint32_t kTraceCapMax = 65536;

struct PairKey {
    uint64_t viewer = 0;
    uint64_t subject = 0;
    bool operator==(const PairKey &o) const { return viewer == o.viewer && subject == o.subject; }
};

struct PairHash {
    size_t operator()(const PairKey &k) const {
        return static_cast<size_t>(k.viewer * 1315423911u) ^ static_cast<size_t>(k.subject);
    }
};

std::atomic<uint64_t> g_fail[static_cast<int>(AoiWriteFrame::Count)]
                           [static_cast<int>(AoiWriteFailReason::Count)];
std::atomic<uint64_t> g_tx[static_cast<int>(AoiWriteFrame::Count)];
std::atomic<uint64_t> g_leave_fail{0};
std::atomic<uint64_t> g_leave_recovered{0};
std::atomic<uint64_t> g_leave_give_up{0};
std::atomic<uint64_t> g_leave_dropped_offline{0};
std::atomic<uint64_t> g_leave_drop[static_cast<int>(AoiLeaveDropReason::Count)];
std::atomic<uint64_t> g_leave_inconsistent{0};
std::vector<int64_t> g_move_frame_events;
std::vector<int64_t> g_move_frame_payload;
std::vector<int64_t> g_move_silence_ms;
std::vector<int64_t> g_visible_per_viewer;
std::atomic<uint64_t> g_egress_control[static_cast<int>(AoiWriteFrame::Count)];
AoiControlBudgetFn g_control_budget_fn = nullptr;
AoiViewerUnsentFn g_unsent_fn = nullptr;
std::atomic<uint64_t> g_tomb_reject{0};
std::atomic<uint64_t> g_tomb_then_enter{0};
std::atomic<uint64_t> g_leave_cells[8][kCellCap];
std::atomic<uint64_t> g_stage[static_cast<int>(AoiMoveStage::Count)][2][16];
std::atomic<uint64_t> g_stage_n[static_cast<int>(AoiMoveStage::Count)][2];
std::atomic<uint64_t> g_present_n{0};
std::atomic<int64_t> g_present_us_max{0};
std::atomic<int> g_present_entities_max{0};

std::atomic<uint64_t> g_busy_samples{0};
std::atomic<uint64_t> g_busy_retrans{0};
std::atomic<uint64_t> g_busy_cwnd_low{0};
std::atomic<uint64_t> g_busy_quiet{0};
std::atomic<uint64_t> g_busy_with_enter{0};
std::atomic<uint64_t> g_busy_with_snapshot{0};
std::atomic<uint64_t> g_busy_tcp_ok{0};
std::atomic<uint64_t> g_enter_sec{0};
std::atomic<uint64_t> g_snap_sec{0};
std::atomic<int64_t> g_sec_bucket{0};

std::mutex g_mu;
std::unordered_map<PairKey, int64_t, PairHash> g_leave_fail_since;

constexpr int kWireHeaderBytes = 66;
constexpr int kBusyMinIntervalMs = 1000;
constexpr size_t kHotSlots = 4096;

struct ConnAcc {
    uint64_t fail = 0;
    uint64_t bytes[5] = {};
};

struct Stall {
    int64_t since_ms = 0;
    int64_t last_tcp_ms = 0;
    int app = 0;
    int unsent = 0;
    int notsent = -1;
    bool tcp_ok = false;
    bool has_backoff = false;
    bool has_last_ack = false;
    bool has_last_sent = false;
    bool has_acked = false;
    bool has_retrans_bytes = false;
    bool has_total_retrans = false;
    bool prev_acked = false;
    bool prev_retrans_bytes = false;
    bool prev_total_retrans = false;
    uint32_t unacked = 0;
    uint32_t retrans = 0;
    uint32_t backoff = 0;
    uint32_t rto_us = 0;
    uint32_t rtt_us = 0;
    uint32_t cwnd = 0;
    uint32_t last_ack_recv_ms = 0;
    uint32_t last_data_sent_ms = 0;
    int64_t acked_delta = -1;
    int64_t retrans_bytes_delta = -1;
    int64_t total_retrans_delta = -1;
    uint64_t prev_acked_v = 0;
    uint64_t prev_retrans_bytes_v = 0;
    uint32_t prev_total_retrans_v = 0;
    sockaddr_storage peer{};
    socklen_t peer_len = 0;
};

std::unordered_map<uint64_t, ConnAcc> g_conn;
std::unordered_map<uint64_t, Stall> g_stall;
std::atomic<uint8_t> g_hot[kHotSlots];
std::atomic<uint64_t> g_stalled_total{0};
std::atomic<uint64_t> g_wire[static_cast<int>(AoiWriteFrame::Count)];
std::atomic<uint64_t> g_tx_frames[static_cast<int>(AoiWriteFrame::Count)];
std::atomic<int64_t> g_now_override{0};
AoiTcpSampleFn g_tcp_fn = nullptr;
AoiConnPlayerFn g_player_fn = nullptr;
AoiViewerOnlineFn g_online_fn = nullptr;

std::vector<int64_t> g_busy_backoff;
std::vector<int64_t> g_busy_last_ack;
std::vector<int64_t> g_busy_last_sent;
std::vector<int64_t> g_busy_acked_delta;
std::vector<int64_t> g_busy_retrans_bytes_delta;
std::vector<int64_t> g_busy_total_retrans_delta;
std::unordered_map<uint64_t, uint8_t> g_tomb_waiting_enter;
std::vector<int64_t> g_leave_recover_ms;
std::vector<int64_t> g_busy_unacked;
std::vector<int64_t> g_busy_retrans_v;
std::vector<int64_t> g_busy_rto;
std::vector<int64_t> g_busy_cwnd;
std::vector<int64_t> g_busy_rtt;
std::vector<int64_t> g_busy_notsent;

thread_local int g_move_timing = 0;

struct TraceSlot {
    AoiTraceFields ev;
};
std::mutex g_trace_mu;
std::vector<TraceSlot> g_trace;
uint32_t g_trace_i = 0;
uint32_t g_trace_n = 0;
int g_trace_on = -1;
int g_trace_sample = 1;
int g_trace_max_viewers = 0;
std::unordered_set<uint64_t> g_trace_viewers;
std::atomic<uint32_t> g_trace_seq{0};

int TraceOn() {
    if (g_trace_on >= 0)
        return g_trace_on;
    const char *e = std::getenv("GAMEMESH_AOI_TRACE");
    g_trace_on = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    uint32_t cap = kTraceCapDefault;
    if (const char *c = std::getenv("GAMEMESH_AOI_TRACE_CAP")) {
        const long v = std::strtol(c, nullptr, 10);
        if (v >= 16 && v <= static_cast<long>(kTraceCapMax))
            cap = static_cast<uint32_t>(v);
    }
    if (const char *s = std::getenv("GAMEMESH_AOI_TRACE_SAMPLE")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v >= 1 && v <= 10000)
            g_trace_sample = static_cast<int>(v);
    }
    if (const char *n = std::getenv("GAMEMESH_AOI_TRACE_MAX_VIEWERS")) {
        const long v = std::strtol(n, nullptr, 10);
        if (v > 0 && v <= 10000)
            g_trace_max_viewers = static_cast<int>(v);
    }
    if (g_trace_on)
        g_trace.resize(cap);
    return g_trace_on;
}

int StageBucket(int64_t us) {
    static const int64_t kEdge[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000,
                                    20000, 50000, 100000, 200000, 500000, 1000000};
    for (int i = 0; i < 14; ++i) {
        if (us <= kEdge[i])
            return i;
    }
    return 14;
}

void BumpSec(int64_t now_ms) {
    const int64_t sec = now_ms / 1000;
    const int64_t prev = g_sec_bucket.load(std::memory_order_relaxed);
    if (prev != sec) {
        g_sec_bucket.store(sec, std::memory_order_relaxed);
        g_enter_sec.store(0, std::memory_order_relaxed);
        g_snap_sec.store(0, std::memory_order_relaxed);
    }
}

void KeepTail(std::vector<int64_t> *v, size_t cap) {
    if (v->size() <= cap)
        return;
    v->erase(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(v->size() - cap));
}

int64_t At(const std::vector<int64_t> &s, double q) {
    if (s.empty())
        return 0;
    size_t idx = static_cast<size_t>(q * static_cast<double>(s.size() - 1));
    if (idx >= s.size())
        idx = s.size() - 1;
    return s[idx];
}

void EmitQuantiles(std::ostream &os, const char *name, const char *help, std::vector<int64_t> s) {
    if (s.empty())
        return;
    std::sort(s.begin(), s.end());
    os << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n";
        os << name << "{quantile=\"0.5\"} " << At(s, 0.50) << "\n";
        os << name << "{quantile=\"0.9\"} " << At(s, 0.90) << "\n";
        os << name << "{quantile=\"0.95\"} " << At(s, 0.95) << "\n";
        os << name << "{quantile=\"0.99\"} " << At(s, 0.99) << "\n";
        os << name << "{quantile=\"1\"} " << s.back() << "\n";
        os << "# HELP " << name << "_count Samples behind the quantiles. Unit: samples.\n";
        os << "# TYPE " << name << "_count gauge\n";
        os << name << "_count " << s.size() << "\n";
    }

}  // namespace

const char *AoiWriteFrameName(AoiWriteFrame frame) {
    switch (frame) {
    case AoiWriteFrame::Enter:
        return "enter";
    case AoiWriteFrame::Leave:
        return "leave";
    case AoiWriteFrame::Move:
        return "move";
    case AoiWriteFrame::Reply:
        return "reply";
    case AoiWriteFrame::Heartbeat:
        return "heartbeat";
    case AoiWriteFrame::Snapshot:
        return "snapshot";
    case AoiWriteFrame::Reliable:
        return "reliable";
    case AoiWriteFrame::Other:
        return "other";
    case AoiWriteFrame::Count:
        break;
    }
    return "other";
}

const char *AoiWriteFailReasonName(AoiWriteFailReason reason) {
    switch (reason) {
    case AoiWriteFailReason::Blocked:
        return "blocked";
    case AoiWriteFailReason::QueueFull:
        return "queue_full";
    case AoiWriteFailReason::TimedWait:
        return "timed_wait";
    case AoiWriteFailReason::Closed:
        return "closed";
    case AoiWriteFailReason::NoSession:
        return "no_session";
    case AoiWriteFailReason::Suppress:
        return "suppress";
    case AoiWriteFailReason::Count:
        break;
    }
    return "other";
}

const char *AoiMoveStageName(AoiMoveStage stage) {
    switch (stage) {
    case AoiMoveStage::SerialQueue:
        return "serial_queue";
    case AoiMoveStage::LockWait:
        return "lock_wait";
    case AoiMoveStage::LockHold:
        return "lock_hold";
    case AoiMoveStage::MoveExec:
        return "move_exec";
    case AoiMoveStage::ReplyQueue:
        return "reply_queue";
    case AoiMoveStage::ReplySend:
        return "reply_send";
    case AoiMoveStage::PresentSlice:
        return "present_slice";
    case AoiMoveStage::Count:
        break;
    }
    return "other";
}

bool AoiTraceEnabled() { return TraceOn() != 0; }

void AoiNoteWriteFail(uint64_t conn_id, AoiWriteFrame frame, AoiWriteFailReason reason) {
    const int f = static_cast<int>(frame);
    const int r = static_cast<int>(reason);
    if (f < 0 || f >= static_cast<int>(AoiWriteFrame::Count) || r < 0 ||
        r >= static_cast<int>(AoiWriteFailReason::Count))
        return;
    g_fail[f][r].fetch_add(1, std::memory_order_relaxed);
    if (conn_id == 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_conn.size() < 8192 || g_conn.count(conn_id) != 0)
        g_conn[conn_id].fail += 1;
}

int FrameBucket(AoiWriteFrame frame) {
    switch (frame) {
    case AoiWriteFrame::Move:
        return 0;
    case AoiWriteFrame::Reply:
        return 1;
    case AoiWriteFrame::Enter:
        return 2;
    case AoiWriteFrame::Leave:
        return 3;
    default:
        return 4;
    }
}

void AoiNoteMoveFrameSample(int events, int payload_bytes) {
    if (events <= 0 || payload_bytes < 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_move_frame_events.push_back(events);
    g_move_frame_payload.push_back(payload_bytes);
    KeepTail(&g_move_frame_events, 4096);
    KeepTail(&g_move_frame_payload, 4096);
}

void AoiSetControlBudgetFn(AoiControlBudgetFn fn) { g_control_budget_fn = fn; }

void AoiNoteEgressControlWire(AoiWriteFrame frame, uint64_t wire_bytes) {
    const int f = static_cast<int>(frame);
    if (f < 0 || f >= static_cast<int>(AoiWriteFrame::Count) || wire_bytes == 0)
        return;
    if (frame == AoiWriteFrame::Move)
        return;
    g_egress_control[f].fetch_add(wire_bytes, std::memory_order_relaxed);
}

void AoiNoteMoveSilenceMs(int64_t silence_ms) {
    if (silence_ms < 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_move_silence_ms.push_back(silence_ms);
    KeepTail(&g_move_silence_ms, 4096);
}

void AoiNoteVisiblePerViewer(int n) {
    if (n < 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_visible_per_viewer.push_back(n);
    KeepTail(&g_visible_per_viewer, 4096);
}

void AoiNoteTxBytes(AoiWriteFrame frame, uint64_t bytes, uint64_t conn_id) {
    const int f = static_cast<int>(frame);
    if (f < 0 || f >= static_cast<int>(AoiWriteFrame::Count) || bytes == 0)
        return;
    g_tx[f].fetch_add(bytes, std::memory_order_relaxed);
    g_tx_frames[f].fetch_add(1, std::memory_order_relaxed);
    g_wire[f].fetch_add(bytes + static_cast<uint64_t>(kWireHeaderBytes), std::memory_order_relaxed);
    if (frame != AoiWriteFrame::Move) {
        AoiNoteEgressControlWire(frame, bytes + static_cast<uint64_t>(kWireHeaderBytes));
        if (g_control_budget_fn && conn_id != 0)
            g_control_budget_fn(conn_id, frame, static_cast<int>(bytes));
    }
    if (conn_id == 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_conn.size() < 8192 || g_conn.count(conn_id) != 0) {
        const int bucket = FrameBucket(frame);
        g_conn[conn_id].bytes[bucket] += bytes;
    }
}

void AoiNoteCoincidentEnter(bool enter, bool snapshot) {
    if (!enter && !snapshot)
        return;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    BumpSec(now_ms);
    if (enter)
        g_enter_sec.fetch_add(1, std::memory_order_relaxed);
    if (snapshot)
        g_snap_sec.fetch_add(1, std::memory_order_relaxed);
}

void AoiNoteLeaveWriteFail(uint64_t viewer_id, uint64_t subject_id, int64_t now_ms) {
    g_leave_fail.fetch_add(1, std::memory_order_relaxed);
    if (viewer_id == 0 || subject_id == 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto &at = g_leave_fail_since[PairKey{viewer_id, subject_id}];
    if (at == 0)
        at = now_ms > 0 ? now_ms : 1;
}

bool AoiTakeLeaveFail(uint64_t viewer_id, uint64_t subject_id, int64_t now_ms, bool record_recovery) {
    if (viewer_id == 0 || subject_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_leave_fail_since.find(PairKey{viewer_id, subject_id});
    if (it == g_leave_fail_since.end())
        return false;
    if (record_recovery) {
        const int64_t dt = now_ms > it->second ? now_ms - it->second : 0;
        g_leave_recovered.fetch_add(1, std::memory_order_relaxed);
        g_leave_recover_ms.push_back(dt);
        KeepTail(&g_leave_recover_ms, 4096);
    }
    g_leave_fail_since.erase(it);
    return true;
}

void AoiCountLeaveDrop(AoiLeaveDropReason reason) {
    const int idx = static_cast<int>(reason);
    if (idx < 0 || idx >= static_cast<int>(AoiLeaveDropReason::Count))
        return;
    g_leave_drop[idx].fetch_add(1, std::memory_order_relaxed);
    if (reason == AoiLeaveDropReason::ViewerOffline)
        g_leave_dropped_offline.fetch_add(1, std::memory_order_relaxed);
    if (reason == AoiLeaveDropReason::GivenUp)
        g_leave_give_up.fetch_add(1, std::memory_order_relaxed);
}

uint64_t AoiLeaveDropCount(AoiLeaveDropReason reason) {
    const int idx = static_cast<int>(reason);
    if (idx < 0 || idx >= static_cast<int>(AoiLeaveDropReason::Count))
        return 0;
    return g_leave_drop[idx].load(std::memory_order_relaxed);
}

void AoiMeasureLeaveInconsistent(const std::vector<std::pair<uint64_t, uint64_t>> &queued_viewer_subject) {
    std::unordered_set<PairKey, PairHash> queued;
    queued.reserve(queued_viewer_subject.size());
    for (const auto &row : queued_viewer_subject)
        queued.insert(PairKey{row.first, row.second});
    uint64_t missing = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto &kv : g_leave_fail_since) {
            if (queued.count(kv.first) == 0)
                ++missing;
        }
    }
    g_leave_inconsistent.store(missing, std::memory_order_relaxed);
}

uint64_t AoiLeavePendingInconsistent() { return g_leave_inconsistent.load(std::memory_order_relaxed); }

void AoiNoteLeaveWriteOk(uint64_t viewer_id, uint64_t subject_id, int64_t now_ms) {
    if (!AoiTakeLeaveFail(viewer_id, subject_id, now_ms, true))
        return;
    AoiCountLeaveDrop(AoiLeaveDropReason::Acked);
}

void AoiNoteLeaveGiveUp(uint64_t viewer_id, uint64_t subject_id) {
    if (viewer_id == 0 || subject_id == 0)
        return;
    AoiTakeLeaveFail(viewer_id, subject_id, 0, false);
    AoiCountLeaveDrop(AoiLeaveDropReason::GivenUp);
}

int64_t AoiLeaveFailSince(uint64_t viewer_id, uint64_t subject_id) {
    if (viewer_id == 0 || subject_id == 0)
        return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_leave_fail_since.find(PairKey{viewer_id, subject_id});
    if (it == g_leave_fail_since.end())
        return 0;
    return it->second;
}

void AoiNoteLeaveDroppedOffline(uint64_t viewer_id, uint64_t subject_id) {
    if (!AoiTakeLeaveFail(viewer_id, subject_id, 0, false))
        return;
    AoiCountLeaveDrop(AoiLeaveDropReason::ViewerOffline);
}

void AoiDropViewerLeavePending(uint64_t viewer_id) {
    if (viewer_id == 0)
        return;
    std::vector<uint64_t> subjects;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto &kv : g_leave_fail_since) {
            if (kv.first.viewer == viewer_id)
                subjects.push_back(kv.first.subject);
        }
    }
    for (uint64_t subject : subjects) {
        if (AoiTakeLeaveFail(viewer_id, subject, 0, false))
            AoiCountLeaveDrop(AoiLeaveDropReason::ViewerOffline);
    }
}

void AoiSetViewerUnsentFnForTest(AoiViewerUnsentFn fn) { g_unsent_fn = fn; }

int AoiViewerUnsentBytes(uint64_t viewer_id) {
    if (viewer_id == 0 || g_unsent_fn == nullptr)
        return -1;
    return g_unsent_fn(viewer_id);
}

void AoiNoteTombstoneReject(uint64_t entity_id) {
    g_tomb_reject.fetch_add(1, std::memory_order_relaxed);
    if (entity_id == 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_tomb_waiting_enter.size() < 8192)
        g_tomb_waiting_enter[entity_id] = 1;
}

void AoiNoteTombstoneEnter(uint64_t entity_id) {
    if (entity_id == 0)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_tomb_waiting_enter.find(entity_id);
    if (it == g_tomb_waiting_enter.end())
        return;
    g_tomb_waiting_enter.erase(it);
    g_tomb_then_enter.fetch_add(1, std::memory_order_relaxed);
}

void AoiNoteLeaveCells(int reason, int chebyshev_cells) {
    if (reason < 0 || reason >= 8 || chebyshev_cells < 0)
        return;
    const int cell = chebyshev_cells >= kCellCap ? kCellCap - 1 : chebyshev_cells;
    g_leave_cells[reason][cell].fetch_add(1, std::memory_order_relaxed);
}

void AoiNoteMoveStage(AoiMoveStage stage, int64_t us, bool unsent_over_16kb, bool unsent_known) {
    const int s = static_cast<int>(stage);
    if (s < 0 || s >= static_cast<int>(AoiMoveStage::Count) || us < 0)
        return;
    const int parked = unsent_known ? (unsent_over_16kb ? 1 : 0) : 0;
    if (!unsent_known && stage != AoiMoveStage::PresentSlice) {
        g_stage[s][0][StageBucket(us)].fetch_add(1, std::memory_order_relaxed);
        g_stage_n[s][0].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_stage[s][parked][StageBucket(us)].fetch_add(1, std::memory_order_relaxed);
    g_stage_n[s][parked].fetch_add(1, std::memory_order_relaxed);
}

void AoiNotePresentSlice(int64_t us, int entities) {
    if (us < 0)
        return;
    AoiNoteMoveStage(AoiMoveStage::PresentSlice, us, false, true);
    g_present_n.fetch_add(1, std::memory_order_relaxed);
    int64_t prev = g_present_us_max.load(std::memory_order_relaxed);
    while (us > prev && !g_present_us_max.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
    int ent_prev = g_present_entities_max.load(std::memory_order_relaxed);
    while (entities > ent_prev &&
           !g_present_entities_max.compare_exchange_weak(ent_prev, entities, std::memory_order_relaxed)) {
    }
}

void AoiEnterMoveTiming() { ++g_move_timing; }
void AoiLeaveMoveTiming() {
    if (g_move_timing > 0)
        --g_move_timing;
}
bool AoiInMoveTiming() { return g_move_timing > 0; }

void AoiTrace(const AoiTraceFields &ev) {
    if (!TraceOn() || g_trace.empty())
        return;
    const uint32_t n = g_trace_seq.fetch_add(1, std::memory_order_relaxed);
    if (g_trace_sample > 1 && (n % static_cast<uint32_t>(g_trace_sample)) != 0)
        return;
    std::lock_guard<std::mutex> lk(g_trace_mu);
    if (g_trace_max_viewers > 0 && ev.viewer != 0) {
        if (g_trace_viewers.count(ev.viewer) == 0) {
            if (static_cast<int>(g_trace_viewers.size()) >= g_trace_max_viewers)
                return;
            g_trace_viewers.insert(ev.viewer);
        }
    }
    if (g_trace.empty())
        return;
    g_trace[g_trace_i].ev = ev;
    g_trace_i = (g_trace_i + 1) % static_cast<uint32_t>(g_trace.size());
    if (g_trace_n < g_trace.size())
        ++g_trace_n;
}

template <typename T>
struct HasTcpNotSent {
    template <typename U>
    static auto probe(int) -> decltype(std::declval<U>().tcpi_notsent_bytes, std::true_type{});
    template <typename>
    static std::false_type probe(...);
    static constexpr bool value = decltype(probe<T>(0))::value;
};

template <typename T>
struct HasTcpBytesRetrans {
    template <typename U>
    static auto probe(int) -> decltype(std::declval<U>().tcpi_bytes_retrans, std::true_type{});
    template <typename>
    static std::false_type probe(...);
    static constexpr bool value = decltype(probe<T>(0))::value;
};

template <typename Info>
void ReadOptionalTcp(const Info &info, socklen_t len, AoiTcpSample *out) {
    if constexpr (HasTcpNotSent<Info>::value) {
        const size_t need = offsetof(Info, tcpi_notsent_bytes) + sizeof(info.tcpi_notsent_bytes);
        if (static_cast<size_t>(len) >= need) {
            out->notsent = static_cast<int>(info.tcpi_notsent_bytes);
            out->has_notsent_ioctl = true;
        }
    }
    if constexpr (HasTcpBytesRetrans<Info>::value) {
        const size_t need = offsetof(Info, tcpi_bytes_retrans) + sizeof(info.tcpi_bytes_retrans);
        if (static_cast<size_t>(len) >= need) {
            out->has_bytes_retrans = true;
            out->bytes_retrans = static_cast<uint64_t>(info.tcpi_bytes_retrans);
        }
    }
}

int64_t EvidenceNowMs() {
    const int64_t forced = g_now_override.load(std::memory_order_relaxed);
    if (forced > 0)
        return forced;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ReadLiveTcp(int fd, AoiTcpSample *out) {
    *out = AoiTcpSample{};
    out->notsent = -1;
    int notsent = 0;
    if (::ioctl(fd, SIOCOUTQNSD, &notsent) == 0 && notsent >= 0) {
        out->notsent = notsent;
        out->has_notsent_ioctl = true;
    }
    tcp_info info;
    std::memset(&info, 0, sizeof(info));
    socklen_t len = sizeof(info);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0)
        return false;
    out->ok = true;
    out->unacked = info.tcpi_unacked;
    out->retrans = info.tcpi_retrans;
    out->retransmitting = info.tcpi_retrans > 0 || info.tcpi_retransmits > 0;
    out->rto_us = info.tcpi_rto;
    out->rtt_us = info.tcpi_rtt;
    out->snd_cwnd = info.tcpi_snd_cwnd;
    if (len >= offsetof(tcp_info, tcpi_backoff) + sizeof(info.tcpi_backoff)) {
        out->has_backoff = true;
        out->backoff = info.tcpi_backoff;
    }
    if (len >= offsetof(tcp_info, tcpi_last_ack_recv) + sizeof(info.tcpi_last_ack_recv)) {
        out->has_last_ack_recv = true;
        out->last_ack_recv_ms = info.tcpi_last_ack_recv;
    }
    if (len >= offsetof(tcp_info, tcpi_last_data_sent) + sizeof(info.tcpi_last_data_sent)) {
        out->has_last_data_sent = true;
        out->last_data_sent_ms = info.tcpi_last_data_sent;
    }
    if (len >= offsetof(tcp_info, tcpi_bytes_acked) + sizeof(info.tcpi_bytes_acked)) {
        out->has_bytes_acked = true;
        out->bytes_acked = info.tcpi_bytes_acked;
    }
    if (len >= offsetof(tcp_info, tcpi_total_retrans) + sizeof(info.tcpi_total_retrans)) {
        out->has_total_retrans = true;
        out->total_retrans = info.tcpi_total_retrans;
    }
    ReadOptionalTcp(info, len, out);
    return true;
}

size_t HotIndex(uint64_t conn_id) { return static_cast<size_t>(conn_id % kHotSlots); }

void MarkHotLocked() {
    uint8_t used[kHotSlots];
    std::memset(used, 0, sizeof(used));
    for (const auto &kv : g_stall)
        used[HotIndex(kv.first)] = 1;
    for (size_t i = 0; i < kHotSlots; ++i)
        g_hot[i].store(used[i], std::memory_order_relaxed);
}

void NoteDelta(bool *have_prev, uint64_t *prev, uint64_t now, int64_t *delta) {
    if (*have_prev)
        *delta = now >= *prev ? static_cast<int64_t>(now - *prev) : 0;
    else
        *delta = -1;
    *prev = now;
    *have_prev = true;
}

void ApplyTcpLocked(Stall *st, const AoiTcpSample &tcp) {
    st->tcp_ok = tcp.ok;
    st->notsent = tcp.has_notsent_ioctl ? tcp.notsent : -1;
    st->has_backoff = tcp.has_backoff;
    st->has_last_ack = tcp.has_last_ack_recv;
    st->has_last_sent = tcp.has_last_data_sent;
    st->has_acked = tcp.has_bytes_acked;
    st->has_retrans_bytes = tcp.has_bytes_retrans;
    st->has_total_retrans = tcp.has_total_retrans;
    if (!tcp.ok)
        return;
    st->unacked = tcp.unacked;
    st->retrans = tcp.retrans;
    st->rto_us = tcp.rto_us;
    st->rtt_us = tcp.rtt_us;
    st->cwnd = tcp.snd_cwnd;
    if (tcp.has_backoff)
        st->backoff = tcp.backoff;
    if (tcp.has_last_ack_recv)
        st->last_ack_recv_ms = tcp.last_ack_recv_ms;
    if (tcp.has_last_data_sent)
        st->last_data_sent_ms = tcp.last_data_sent_ms;
    if (tcp.has_bytes_acked)
        NoteDelta(&st->prev_acked, &st->prev_acked_v, tcp.bytes_acked, &st->acked_delta);
    if (tcp.has_bytes_retrans)
        NoteDelta(&st->prev_retrans_bytes, &st->prev_retrans_bytes_v, tcp.bytes_retrans,
                  &st->retrans_bytes_delta);
    if (tcp.has_total_retrans) {
        uint64_t prev = st->prev_total_retrans_v;
        NoteDelta(&st->prev_total_retrans, &prev, tcp.total_retrans, &st->total_retrans_delta);
        st->prev_total_retrans_v = static_cast<uint32_t>(prev);
    }
}

void PushBusyLocked(const Stall &st) {
    if (st.tcp_ok) {
        g_busy_unacked.push_back(st.unacked);
        g_busy_retrans_v.push_back(st.retrans);
        g_busy_rto.push_back(st.rto_us);
        g_busy_cwnd.push_back(st.cwnd);
        g_busy_rtt.push_back(st.rtt_us);
        if (st.has_backoff)
            g_busy_backoff.push_back(st.backoff);
        if (st.has_last_ack)
            g_busy_last_ack.push_back(st.last_ack_recv_ms);
        if (st.has_last_sent)
            g_busy_last_sent.push_back(st.last_data_sent_ms);
        if (st.acked_delta >= 0)
            g_busy_acked_delta.push_back(st.acked_delta);
        if (st.retrans_bytes_delta >= 0)
            g_busy_retrans_bytes_delta.push_back(st.retrans_bytes_delta);
        if (st.total_retrans_delta >= 0)
            g_busy_total_retrans_delta.push_back(st.total_retrans_delta);
    }
    if (st.notsent >= 0)
        g_busy_notsent.push_back(st.notsent);
    KeepTail(&g_busy_unacked, 2048);
    KeepTail(&g_busy_retrans_v, 2048);
    KeepTail(&g_busy_rto, 2048);
    KeepTail(&g_busy_cwnd, 2048);
    KeepTail(&g_busy_rtt, 2048);
    KeepTail(&g_busy_notsent, 2048);
    KeepTail(&g_busy_backoff, 2048);
    KeepTail(&g_busy_last_ack, 2048);
    KeepTail(&g_busy_last_sent, 2048);
    KeepTail(&g_busy_acked_delta, 2048);
    KeepTail(&g_busy_retrans_bytes_delta, 2048);
    KeepTail(&g_busy_total_retrans_delta, 2048);
}

std::unordered_set<uint64_t> g_stalled_seen;

void AoiSetTcpSampleFnForTest(AoiTcpSampleFn fn) { g_tcp_fn = fn; }
void AoiSetEvidenceNowMsForTest(int64_t ms) { g_now_override.store(ms, std::memory_order_relaxed); }
void AoiSetConnPlayerFn(AoiConnPlayerFn fn) { g_player_fn = fn; }
void AoiSetViewerOnlineFn(AoiViewerOnlineFn fn) { g_online_fn = fn; }

void AoiObserveBusySocket(uint64_t conn_id, int fd, int app_bytes, int unsent_bytes) {
    const int pending = (app_bytes > 0 ? app_bytes : 0) + (unsent_bytes > 0 ? unsent_bytes : 0);
    const bool over = pending > 16 * 1024;
    if (conn_id == 0 || (!over && g_hot[HotIndex(conn_id)].load(std::memory_order_relaxed) == 0))
        return;
    if (!g_mu.try_lock())
        return;
    struct Unlock {
        ~Unlock() { g_mu.unlock(); }
    } unlock;

    if (!over) {
        if (g_stall.erase(conn_id) != 0)
            MarkHotLocked();
        return;
    }
    const int64_t now_ms = EvidenceNowMs();
    Stall &st = g_stall[conn_id];
    if (st.since_ms == 0) {
        st.since_ms = now_ms > 0 ? now_ms : 1;
        if (g_stalled_seen.size() < 8192 && g_stalled_seen.insert(conn_id).second)
            g_stalled_total.fetch_add(1, std::memory_order_relaxed);
        g_hot[HotIndex(conn_id)].store(1, std::memory_order_relaxed);
        if (g_tcp_fn == nullptr && fd >= 0 && st.peer_len == 0) {
            socklen_t sl = sizeof(st.peer);
            if (::getpeername(fd, reinterpret_cast<sockaddr *>(&st.peer), &sl) == 0)
                st.peer_len = sl;
        }
    }
    st.app = app_bytes > 0 ? app_bytes : 0;
    st.unsent = unsent_bytes > 0 ? unsent_bytes : 0;
    if (st.last_tcp_ms != 0 && now_ms >= st.last_tcp_ms && now_ms - st.last_tcp_ms < kBusyMinIntervalMs)
        return;
    st.last_tcp_ms = now_ms;
    const int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    BumpSec(wall_ms);
    if (g_enter_sec.load(std::memory_order_relaxed) > 0)
        g_busy_with_enter.fetch_add(1, std::memory_order_relaxed);
    if (g_snap_sec.load(std::memory_order_relaxed) > 0)
        g_busy_with_snapshot.fetch_add(1, std::memory_order_relaxed);
    g_busy_samples.fetch_add(1, std::memory_order_relaxed);

    AoiTcpSample tcp;
    const bool got = g_tcp_fn != nullptr ? g_tcp_fn(fd, &tcp) : (fd >= 0 && ReadLiveTcp(fd, &tcp));
    if (!got) {
        st.tcp_ok = false;
        st.notsent = -1;
        return;
    }
    ApplyTcpLocked(&st, tcp);
    if (tcp.ok) {
        g_busy_tcp_ok.fetch_add(1, std::memory_order_relaxed);
        const bool retrans = tcp.retransmitting || tcp.retrans > 0;
        const bool cwnd_low = tcp.snd_cwnd > 0 && tcp.snd_cwnd <= 2;
        if (retrans)
            g_busy_retrans.fetch_add(1, std::memory_order_relaxed);
        else if (cwnd_low)
            g_busy_cwnd_low.fetch_add(1, std::memory_order_relaxed);
        else
            g_busy_quiet.fetch_add(1, std::memory_order_relaxed);
    }
    PushBusyLocked(st);
}

void AoiEvidenceWritePrometheus(std::ostream &os) {
    os << "# HELP gamemesh_write_fail_total Frames that did not enter the connection send buffer. Unit: events.\n"
          "# TYPE gamemesh_write_fail_total counter\n";
    for (int f = 0; f < static_cast<int>(AoiWriteFrame::Count); ++f) {
        for (int r = 0; r < static_cast<int>(AoiWriteFailReason::Count); ++r) {
            const uint64_t n = g_fail[f][r].load(std::memory_order_relaxed);
            os << "gamemesh_write_fail_total{frame=\"" << AoiWriteFrameName(static_cast<AoiWriteFrame>(f))
               << "\",reason=\"" << AoiWriteFailReasonName(static_cast<AoiWriteFailReason>(r)) << "\"} " << n
               << "\n";
        }
    }
    os << "# HELP gamemesh_tx_bytes_by_frame Bytes accepted into the connection, by frame kind. Unit: bytes.\n"
          "# TYPE gamemesh_tx_bytes_by_frame counter\n";
    for (int f = 0; f < static_cast<int>(AoiWriteFrame::Count); ++f) {
        const uint64_t n = g_tx[f].load(std::memory_order_relaxed);
        if (n == 0)
            continue;
        os << "gamemesh_tx_bytes_by_frame{frame=\"" << AoiWriteFrameName(static_cast<AoiWriteFrame>(f))
           << "\"} " << n << "\n";
    }
    os << "# HELP gamemesh_tx_frames_by_frame Frames accepted into the connection, by frame kind. Unit: frames.\n"
          "# TYPE gamemesh_tx_frames_by_frame counter\n";
    os << "# HELP gamemesh_egress_wire_bytes_estimate Payload bytes plus kOnWireHeaderBytes (66) from AoiMoveCoalesceLedger.h. 66 is ethernet 14 plus IPv4 20 plus TCP 20 plus 12 bytes of options. One accepted frame adds one header. Unit: bytes.\n"
          "# TYPE gamemesh_egress_wire_bytes_estimate counter\n";
    for (int f = 0; f < static_cast<int>(AoiWriteFrame::Count); ++f) {
        const char *name = AoiWriteFrameName(static_cast<AoiWriteFrame>(f));
        os << "gamemesh_tx_frames_by_frame{frame=\"" << name << "\"} "
           << g_tx_frames[f].load(std::memory_order_relaxed) << "\n";
        os << "gamemesh_egress_wire_bytes_estimate{frame=\"" << name << "\"} "
           << g_wire[f].load(std::memory_order_relaxed) << "\n";
    }
    std::vector<int64_t> move_events;
    std::vector<int64_t> move_payload;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        move_events = g_move_frame_events;
        move_payload = g_move_frame_payload;
    }
    EmitQuantiles(os, "gamemesh_move_frame_events",
                  "Events in one MOVE frame accepted by the connection. One sample is one frame. Capped at 4096. Unit: events.",
                  move_events);
    EmitQuantiles(os, "gamemesh_move_frame_payload_bytes",
                  "Payload bytes of one MOVE frame accepted by the connection, including the 4 byte length prefix. One sample is one frame. Capped at 4096. Unit: bytes.",
                  move_payload);
    os << "# HELP gamemesh_egress_control_bytes_total Control-frame on-wire bytes charged against the per-connection egress window (payload plus 66). Unit: bytes.\n"
          "# TYPE gamemesh_egress_control_bytes_total counter\n";
    for (int f = 0; f < static_cast<int>(AoiWriteFrame::Count); ++f) {
        if (static_cast<AoiWriteFrame>(f) == AoiWriteFrame::Move)
            continue;
        const uint64_t n = g_egress_control[f].load(std::memory_order_relaxed);
        os << "gamemesh_egress_control_bytes_total{frame=\"" << AoiWriteFrameName(static_cast<AoiWriteFrame>(f))
           << "\"} " << n << "\n";
    }
    os << "# HELP gamemesh_egress_budget_exhausted_total Times Pick returned empty because the per-connection move budget was exhausted. Unit: events.\n"
          "# TYPE gamemesh_egress_budget_exhausted_total counter\n"
       << "gamemesh_egress_budget_exhausted_total "
       << ServerStats::egress_budget_exhausted.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_egress_move_wire_bytes_total On-wire bytes for AOI MOVE confirmed into the kernel (payload plus 66). Unit: bytes.\n"
          "# TYPE gamemesh_egress_move_wire_bytes_total counter\n"
       << "gamemesh_egress_move_wire_bytes_total "
       << ServerStats::egress_move_wire_bytes.load(std::memory_order_relaxed) << "\n";
    std::vector<int64_t> silence;
    std::vector<int64_t> visible;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        silence = g_move_silence_ms;
        visible = g_visible_per_viewer;
    }
    EmitQuantiles(os, "gamemesh_egress_move_silence_ms",
                  "Silence between two kernel-confirmed position writes for the same entity on one connection. Unit: milliseconds.",
                  silence);
    EmitQuantiles(os, "gamemesh_aoi_visible_per_viewer",
                  "Visible entity count per viewer sample. Unit: entities.", visible);
    os << "# HELP gamemesh_aoi_leave_write_fail_total LEAVE pushes the gateway did not accept. Unit: events.\n"
          "# TYPE gamemesh_aoi_leave_write_fail_total counter\n"
       << "gamemesh_aoi_leave_write_fail_total " << g_leave_fail.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_leave_write_recovered_total Failed LEAVE that a later send accepted. Unit: events.\n"
          "# TYPE gamemesh_aoi_leave_write_recovered_total counter\n"
       << "gamemesh_aoi_leave_write_recovered_total " << g_leave_recovered.load(std::memory_order_relaxed)
       << "\n";
    os << "# HELP gamemesh_aoi_leave_write_given_up_total Failed LEAVE abandoned before a later send was accepted. Unit: events.\n"
          "# TYPE gamemesh_aoi_leave_write_given_up_total counter\n"
       << "gamemesh_aoi_leave_write_given_up_total " << g_leave_give_up.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_leave_write_dropped_total Failed LEAVE dropped because the viewer was offline. Unit: events.\n"
          "# TYPE gamemesh_aoi_leave_write_dropped_total counter\n"
       << "gamemesh_aoi_leave_write_dropped_total{reason=\"viewer_offline\"} "
       << g_leave_dropped_offline.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_leave_pending_drop_total LEAVE pending pairs removed from the retry queue and the evidence table. Unit: pairs.\n"
          "# TYPE gamemesh_aoi_leave_pending_drop_total counter\n";
    const char *drop_names[] = {"viewer_offline", "back_in_window", "version_superseded", "acked", "given_up"};
    for (int i = 0; i < static_cast<int>(AoiLeaveDropReason::Count); ++i) {
        os << "gamemesh_aoi_leave_pending_drop_total{reason=\"" << drop_names[i] << "\"} "
           << g_leave_drop[i].load(std::memory_order_relaxed) << "\n";
    }
    os << "# HELP gamemesh_aoi_leave_pending_inconsistent Evidence rows whose pair is absent from the leave retry queue. Unit: pairs.\n"
          "# TYPE gamemesh_aoi_leave_pending_inconsistent gauge\n"
       << "gamemesh_aoi_leave_pending_inconsistent " << g_leave_inconsistent.load(std::memory_order_relaxed)
       << "\n";
    os << "# HELP gamemesh_aoi_tombstone_move_reject_total MOVE dropped by the post-LEAVE tombstone. Unit: events.\n"
          "# TYPE gamemesh_aoi_tombstone_move_reject_total counter\n"
       << "gamemesh_aoi_tombstone_move_reject_total " << g_tomb_reject.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_aoi_tombstone_reject_then_enter_total Tombstone-rejected entities that later got ENTER. Unit: entities.\n"
          "# TYPE gamemesh_aoi_tombstone_reject_then_enter_total counter\n"
       << "gamemesh_aoi_tombstone_reject_then_enter_total " << g_tomb_then_enter.load(std::memory_order_relaxed)
       << "\n";

    os << "# HELP gamemesh_aoi_leave_cells LEAVE events by reason and chebyshev cell distance. cells=8 means 8 or more. Unit: events.\n"
          "# TYPE gamemesh_aoi_leave_cells counter\n";
    static const char *kReason[] = {"out_of_window", "cap_evict", "disconnect", "remove_interest",
                                    "retry_resend", "reconnect_resync", "other", "other"};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < kCellCap; ++c) {
            const uint64_t n = g_leave_cells[r][c].load(std::memory_order_relaxed);
            if (n == 0)
                continue;
            os << "gamemesh_aoi_leave_cells{reason=\"" << kReason[r] << "\",cells=\"" << c << "\"} " << n
               << "\n";
        }
    }

    os << "# HELP gamemesh_move_stage_us Move path stage samples. parked=1 means that connection's unsent queue was above 16KB. parked is omitted when the socket is not in this process. Unit: microseconds.\n"
          "# TYPE gamemesh_move_stage_us histogram\n";
    static const char *kUs[] = {"50", "100", "200", "500", "1000", "2000", "5000", "10000",
                                "20000", "50000", "100000", "200000", "500000", "1000000", "+Inf"};
    for (int s = 0; s < static_cast<int>(AoiMoveStage::Count); ++s) {
        const bool socket_known = s == static_cast<int>(AoiMoveStage::ReplyQueue) ||
                                  s == static_cast<int>(AoiMoveStage::ReplySend);
        for (int parked = 0; parked < (socket_known ? 2 : 1); ++parked) {
            if (g_stage_n[s][parked].load(std::memory_order_relaxed) == 0)
                continue;
            const char *parked_label = socket_known ? (parked ? "1" : "0") : "na";
            uint64_t acc = 0;
            for (int b = 0; b < 15; ++b) {
                acc += g_stage[s][parked][b].load(std::memory_order_relaxed);
                os << "gamemesh_move_stage_us_bucket{stage=\"" << AoiMoveStageName(static_cast<AoiMoveStage>(s))
                   << "\",parked=\"" << parked_label << "\",le=\"" << kUs[b] << "\"} " << acc << "\n";
            }
            os << "gamemesh_move_stage_us_count{stage=\"" << AoiMoveStageName(static_cast<AoiMoveStage>(s))
               << "\",parked=\"" << parked_label << "\"} "
               << g_stage_n[s][parked].load(std::memory_order_relaxed) << "\n";
        }
    }
    if (g_present_n.load(std::memory_order_relaxed) > 0) {
        os << "# HELP gamemesh_aoi_present_slice_entities_max Largest entity count in one AdvancePresented slice. Unit: entities.\n"
              "# TYPE gamemesh_aoi_present_slice_entities_max gauge\n"
           << "gamemesh_aoi_present_slice_entities_max " << g_present_entities_max.load(std::memory_order_relaxed)
           << "\n";
    }

    std::vector<int64_t> fails;
    std::vector<std::pair<uint64_t, uint64_t>> top;
    std::vector<int64_t> tx;
    std::vector<int64_t> recover;
    std::vector<int64_t> unacked, retrans, rto, cwnd, rtt, notsent;
    std::vector<int64_t> backoff, last_ack, last_sent, acked_delta, retrans_delta, total_retrans_delta;
    std::vector<std::pair<uint64_t, int64_t>> pending_pairs;
    std::vector<int64_t> stall_age;
    int64_t now_ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        fails.reserve(g_conn.size());
        top.reserve(g_conn.size());
        tx.reserve(g_conn.size());
        for (const auto &kv : g_conn) {
            if (kv.second.fail > 0) {
                fails.push_back(static_cast<int64_t>(kv.second.fail));
                top.push_back(std::make_pair(kv.first, kv.second.fail));
            }
            uint64_t sum = 0;
            for (uint64_t b : kv.second.bytes)
                sum += b;
            if (sum > 0)
                tx.push_back(static_cast<int64_t>(sum));
        }
        recover = g_leave_recover_ms;
        pending_pairs.reserve(g_leave_fail_since.size());
        for (const auto &kv : g_leave_fail_since)
            pending_pairs.push_back(std::make_pair(kv.first.viewer, kv.second));
        unacked = g_busy_unacked;
        retrans = g_busy_retrans_v;
        rto = g_busy_rto;
        cwnd = g_busy_cwnd;
        rtt = g_busy_rtt;
        notsent = g_busy_notsent;
        backoff = g_busy_backoff;
        last_ack = g_busy_last_ack;
        last_sent = g_busy_last_sent;
        acked_delta = g_busy_acked_delta;
        retrans_delta = g_busy_retrans_bytes_delta;
        total_retrans_delta = g_busy_total_retrans_delta;
        now_ms = EvidenceNowMs();
        stall_age.reserve(g_stall.size());
        for (const auto &kv : g_stall) {
            if (kv.second.since_ms > 0 && now_ms >= kv.second.since_ms)
                stall_age.push_back(now_ms - kv.second.since_ms);
        }
    }
    uint64_t pending_online = 0;
    uint64_t pending_offline = 0;
    int64_t oldest = -1;
    int64_t oldest_online = -1;
    int64_t oldest_offline = -1;
    const bool know_online = g_online_fn != nullptr;
    for (const auto &row : pending_pairs) {
        if (oldest < 0 || row.second < oldest)
            oldest = row.second;
        if (!know_online)
            continue;
        bool online = false;
        if (g_online_fn(row.first, &online) && online) {
            ++pending_online;
            if (oldest_online < 0 || row.second < oldest_online)
                oldest_online = row.second;
        } else {
            ++pending_offline;
            if (oldest_offline < 0 || row.second < oldest_offline)
                oldest_offline = row.second;
        }
    }
    os << "# HELP gamemesh_aoi_leave_write_pending Failed LEAVE not yet recovered or given up. Unit: pairs.\n"
          "# TYPE gamemesh_aoi_leave_write_pending gauge\n"
       << "gamemesh_aoi_leave_write_pending " << pending_pairs.size() << "\n";
    if (know_online) {
        os << "# HELP gamemesh_aoi_leave_write_pending_viewer Failed LEAVE split by whether the viewer is still connected. Unit: pairs.\n"
              "# TYPE gamemesh_aoi_leave_write_pending_viewer gauge\n";
        os << "gamemesh_aoi_leave_write_pending_viewer{viewer=\"online\"} " << pending_online << "\n";
        os << "gamemesh_aoi_leave_write_pending_viewer{viewer=\"offline\"} " << pending_offline << "\n";
    }
    if (oldest >= 0 && now_ms >= oldest) {
        os << "# HELP gamemesh_aoi_leave_write_pending_oldest_ms Age of the oldest failed LEAVE still pending. Unit: milliseconds.\n"
              "# TYPE gamemesh_aoi_leave_write_pending_oldest_ms gauge\n"
           << "gamemesh_aoi_leave_write_pending_oldest_ms " << (now_ms - oldest) << "\n";
    }
    if (know_online && (oldest_online >= 0 || oldest_offline >= 0)) {
        os << "# HELP gamemesh_aoi_leave_write_pending_oldest_ms_viewer Age of the oldest failed LEAVE split by viewer connectivity. Empty side is omitted. Unit: milliseconds.\n"
              "# TYPE gamemesh_aoi_leave_write_pending_oldest_ms gauge\n";
        if (oldest_online >= 0 && now_ms >= oldest_online) {
            os << "gamemesh_aoi_leave_write_pending_oldest_ms{viewer=\"online\"} "
               << (now_ms - oldest_online) << "\n";
        }
        if (oldest_offline >= 0 && now_ms >= oldest_offline) {
            os << "gamemesh_aoi_leave_write_pending_oldest_ms{viewer=\"offline\"} "
               << (now_ms - oldest_offline) << "\n";
        }
    }
    os << "# HELP gamemesh_stalled_conns Connections whose kernel unsent plus app buffer is above 16KB right now. Unit: connections.\n"
          "# TYPE gamemesh_stalled_conns gauge\n"
       << "gamemesh_stalled_conns " << stall_age.size() << "\n";
    os << "# HELP gamemesh_stalled_conn_total Distinct connections that have entered the stalled state. Capped at 8192. Unit: connections.\n"
          "# TYPE gamemesh_stalled_conn_total counter\n"
       << "gamemesh_stalled_conn_total " << g_stalled_total.load(std::memory_order_relaxed) << "\n";
    EmitQuantiles(os, "gamemesh_stalled_conn_age_ms",
                  "Age of each connection currently above 16KB unsent. One sample is one connection at scrape time. Unit: milliseconds.",
                  stall_age);
    if (fails.empty()) {
        os << "# HELP gamemesh_write_fail_per_conn_count Connections in the write_fail_per_conn quantiles. A connection is included after its first write failure. Unit: connections.\n"
              "# TYPE gamemesh_write_fail_per_conn_count gauge\n"
           << "gamemesh_write_fail_per_conn_count 0\n";
    } else {
        EmitQuantiles(os, "gamemesh_write_fail_per_conn",
                      "Write failures accumulated on one connection. One sample is one connection that has failed at least once. Connections with zero failures are omitted. Unit: events.",
                      fails);
        std::sort(top.begin(), top.end(),
                  [](const std::pair<uint64_t, uint64_t> &a, const std::pair<uint64_t, uint64_t> &b) {
                      return a.second > b.second;
                  });
        os << "# HELP gamemesh_write_fail_conn_top Connections with the most write failures. Unit: events.\n"
              "# TYPE gamemesh_write_fail_conn_top gauge\n";
        const size_t n = std::min<size_t>(10, top.size());
        for (size_t i = 0; i < n; ++i) {
            os << "gamemesh_write_fail_conn_top{rank=\"" << (i + 1) << "\",conn=\"" << top[i].first << "\"} "
               << top[i].second << "\n";
        }
    }
    if (tx.empty()) {
        os << "# HELP gamemesh_conn_tx_bytes_count Connections in the conn_tx_bytes quantiles. Unit: connections.\n"
              "# TYPE gamemesh_conn_tx_bytes_count gauge\n"
           << "gamemesh_conn_tx_bytes_count 0\n";
    } else {
        EmitQuantiles(os, "gamemesh_conn_tx_bytes",
                      "Bytes accepted on one connection during this process. One sample is one connection. Unit: bytes.",
                      tx);
    }
    EmitQuantiles(os, "gamemesh_aoi_leave_recover_ms",
                  "Milliseconds from the first failed LEAVE write to a later accepted write. Unit: milliseconds.",
                  recover);
    if (g_busy_samples.load(std::memory_order_relaxed) > 0) {
        os << "# HELP gamemesh_unsent16_samples_total Samples of a connection whose app buffer plus kernel unsent exceeded 16KB. At most one TCP_INFO read per second per connection. Unit: samples.\n"
              "# TYPE gamemesh_unsent16_samples_total counter\n"
           << "gamemesh_unsent16_samples_total " << g_busy_samples.load(std::memory_order_relaxed) << "\n";
        os << "# HELP gamemesh_unsent16_class_total How those samples looked. retrans is TCP retransmission. cwnd_low is snd_cwnd at most 2. quiet_tcp has neither. Unit: samples.\n"
              "# TYPE gamemesh_unsent16_class_total counter\n";
        os << "gamemesh_unsent16_class_total{class=\"retrans\"} " << g_busy_retrans.load(std::memory_order_relaxed)
           << "\n";
        os << "gamemesh_unsent16_class_total{class=\"cwnd_low\"} " << g_busy_cwnd_low.load(std::memory_order_relaxed)
           << "\n";
        os << "gamemesh_unsent16_class_total{class=\"quiet_tcp\"} " << g_busy_quiet.load(std::memory_order_relaxed)
           << "\n";
        os << "# HELP gamemesh_unsent16_with_enter_total Samples taken in a second that also accepted an ENTER frame. Unit: samples.\n"
              "# TYPE gamemesh_unsent16_with_enter_total counter\n"
           << "gamemesh_unsent16_with_enter_total " << g_busy_with_enter.load(std::memory_order_relaxed) << "\n";
        os << "# HELP gamemesh_unsent16_with_snapshot_total Samples taken in a second that also accepted a full snapshot. Unit: samples.\n"
              "# TYPE gamemesh_unsent16_with_snapshot_total counter\n"
           << "gamemesh_unsent16_with_snapshot_total " << g_busy_with_snapshot.load(std::memory_order_relaxed)
           << "\n";
        os << "# HELP gamemesh_unsent16_tcp_ok_total Samples that read TCP_INFO. Unit: samples.\n"
              "# TYPE gamemesh_unsent16_tcp_ok_total counter\n"
           << "gamemesh_unsent16_tcp_ok_total " << g_busy_tcp_ok.load(std::memory_order_relaxed) << "\n";
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_unacked",
                      "tcpi_unacked while unsent exceeded 16KB. One sample is one TCP_INFO read. Unit: segments.",
                      unacked);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_retrans",
                      "tcpi_retrans while unsent exceeded 16KB. One sample is one TCP_INFO read. Unit: segments.",
                      retrans);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_rto_us",
                      "tcpi_rto while unsent exceeded 16KB. One sample is one TCP_INFO read. Unit: microseconds.",
                      rto);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_snd_cwnd",
                      "tcpi_snd_cwnd while unsent exceeded 16KB. One sample is one TCP_INFO read. Unit: segments.",
                      cwnd);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_rtt_us",
                      "tcpi_rtt while unsent exceeded 16KB. One sample is one TCP_INFO read. Unit: microseconds.",
                      rtt);
        EmitQuantiles(os, "gamemesh_unsent16_notsent_bytes",
                      "Bytes not yet handed to the NIC, from SIOCOUTQNSD. This header has no tcpi_notsent_bytes. One sample is one read. Unit: bytes.",
                      notsent);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_backoff",
                      "tcpi_backoff while unsent exceeded 16KB. Omitted when the kernel did not return the field. Unit: steps.",
                      backoff);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_last_ack_recv_ms",
                      "tcpi_last_ack_recv while unsent exceeded 16KB. Unit: milliseconds.", last_ack);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_last_data_sent_ms",
                      "tcpi_last_data_sent while unsent exceeded 16KB. Unit: milliseconds.", last_sent);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_bytes_acked_delta",
                      "Increase of tcpi_bytes_acked between samples. Omitted when the kernel struct did not include the field. Unit: bytes.",
                      acked_delta);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_bytes_retrans_delta",
                      "Increase of tcpi_bytes_retrans between samples. Omitted when the header has no such field. Unit: bytes.",
                      retrans_delta);
        EmitQuantiles(os, "gamemesh_unsent16_tcpi_total_retrans_delta",
                      "Increase of tcpi_total_retrans between samples. This is the substitute for tcpi_bytes_retrans on headers that lack that field. Unit: segments.",
                      total_retrans_delta);
    }
}

std::string AoiTraceJson(uint64_t viewer, uint64_t subject, int limit) {
    if (!TraceOn())
        return "{\"enabled\":false,\"events\":[]}\n";
    if (limit <= 0)
        limit = 50;
    if (limit > 500)
        limit = 500;
    std::vector<AoiTraceFields> rows;
    {
        std::lock_guard<std::mutex> lk(g_trace_mu);
        if (g_trace.empty() || g_trace_n == 0)
            return "{\"enabled\":true,\"events\":[]}\n";
        const uint32_t cap = static_cast<uint32_t>(g_trace.size());
        for (uint32_t n = 0; n < g_trace_n; ++n) {
            const uint32_t idx = (g_trace_i + cap - 1 - n) % cap;
            const AoiTraceFields &ev = g_trace[idx].ev;
            if (viewer != 0 && ev.viewer != viewer)
                continue;
            if (subject != 0 && ev.subject != subject)
                continue;
            rows.push_back(ev);
            if (static_cast<int>(rows.size()) >= limit)
                break;
        }
    }
    std::string out = "{\"enabled\":true,\"events\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const AoiTraceFields &ev = rows[i];
        if (i)
            out.push_back(',');
        const std::string offer = ev.offer_ms > 0 ? std::to_string(ev.offer_ms) : "null";
        const std::string buffer = ev.buffer_ms > 0 ? std::to_string(ev.buffer_ms) : "null";
        const std::string kernel = ev.kernel_ms > 0 ? std::to_string(ev.kernel_ms) : "null";
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      "{\"viewer\":%llu,\"subject\":%llu,\"op\":%d,\"version\":%llu,"
                      "\"offer_ms\":%s,\"buffer_ms\":%s,\"kernel_ms\":%s,\"fail\":%d}",
                      static_cast<unsigned long long>(ev.viewer),
                      static_cast<unsigned long long>(ev.subject), ev.op,
                      static_cast<unsigned long long>(ev.version), offer.c_str(), buffer.c_str(),
                      kernel.c_str(), ev.fail);
        out += buf;
    }
    out += "]}\n";
    return out;
}

namespace {

std::string PeerText(const sockaddr_storage &addr, socklen_t len) {
    if (len == 0)
        return "null";
    char host[64];
    host[0] = '\0';
    unsigned port = 0;
    if (addr.ss_family == AF_INET && len >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(&addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) == nullptr)
            return "null";
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6 && len >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) == nullptr)
            return "null";
        port = ntohs(in6->sin6_port);
    } else {
        return "null";
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "\"%s:%u\"", host, port);
    return buf;
}

}  // namespace

std::string AoiStalledJson(int n) {
    if (n <= 0)
        n = 20;
    if (n > 100)
        n = 100;
    struct Row {
        uint64_t conn = 0;
        Stall st;
        uint64_t fail = 0;
        uint64_t bytes[5] = {};
    };
    std::vector<Row> rows;
    int64_t now_ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        now_ms = EvidenceNowMs();
        rows.reserve(g_stall.size());
        for (const auto &kv : g_stall) {
            Row row;
            row.conn = kv.first;
            row.st = kv.second;
            auto it = g_conn.find(kv.first);
            if (it != g_conn.end()) {
                row.fail = it->second.fail;
                for (int i = 0; i < 5; ++i)
                    row.bytes[i] = it->second.bytes[i];
            }
            rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.st.unsent != b.st.unsent)
            return a.st.unsent > b.st.unsent;
        return a.conn < b.conn;
    });
    if (static_cast<int>(rows.size()) > n)
        rows.resize(static_cast<size_t>(n));
    std::string out = "{\"conns\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &row = rows[i];
        uint64_t player = 0;
        const bool have_player = g_player_fn != nullptr && g_player_fn(row.conn, &player);
        const int64_t age = row.st.since_ms > 0 && now_ms >= row.st.since_ms ? now_ms - row.st.since_ms : 0;
        uint64_t sum = 0;
        for (uint64_t b : row.bytes)
            sum += b;
        if (i)
            out.push_back(',');
        auto add_u = [&](const char *key, bool has, uint64_t v) {
            out += "\"";
            out += key;
            out += "\":";
            if (!has)
                out += "null";
            else
                out += std::to_string(v);
        };
        auto add_i = [&](const char *key, int64_t v) {
            out += "\"";
            out += key;
            out += "\":";
            if (v < 0)
                out += "null";
            else
                out += std::to_string(v);
        };
        out += "{\"conn_id\":" + std::to_string(row.conn) + ",\"player_id\":";
        out += have_player ? std::to_string(player) : "null";
        out += ",\"peer\":" + PeerText(row.st.peer, row.st.peer_len);
        out += ",\"unsent_bytes\":" + std::to_string(row.st.unsent);
        out += ",\"app_bytes\":" + std::to_string(row.st.app) + ",";
        add_u("tcpi_unacked", row.st.tcp_ok, row.st.unacked);
        out.push_back(',');
        add_u("tcpi_retrans", row.st.tcp_ok, row.st.retrans);
        out.push_back(',');
        add_u("tcpi_backoff", row.st.has_backoff, row.st.backoff);
        out.push_back(',');
        add_u("tcpi_rto_us", row.st.tcp_ok, row.st.rto_us);
        out.push_back(',');
        add_u("tcpi_rtt_us", row.st.tcp_ok, row.st.rtt_us);
        out.push_back(',');
        add_u("tcpi_snd_cwnd", row.st.tcp_ok, row.st.cwnd);
        out.push_back(',');
        add_u("tcpi_last_ack_recv_ms", row.st.has_last_ack, row.st.last_ack_recv_ms);
        out.push_back(',');
        add_u("tcpi_last_data_sent_ms", row.st.has_last_sent, row.st.last_data_sent_ms);
        out.push_back(',');
        add_i("tcpi_bytes_acked_delta", row.st.has_acked ? row.st.acked_delta : -1);
        out.push_back(',');
        add_i("tcpi_bytes_retrans_delta", row.st.has_retrans_bytes ? row.st.retrans_bytes_delta : -1);
        out.push_back(',');
        add_i("tcpi_total_retrans_delta", row.st.has_total_retrans ? row.st.total_retrans_delta : -1);
        out.push_back(',');
        add_i("notsent_bytes", row.st.notsent);
        out += ",\"notsent_source\":\"SIOCOUTQNSD\",\"age_ms\":" + std::to_string(age);
        out += ",\"write_fail\":" + std::to_string(row.fail) + ",";
        out += "\"bytes\":{";
        const char *names[] = {"move", "reply", "enter", "leave", "other"};
        for (int b = 0; b < 5; ++b) {
            if (b)
                out.push_back(',');
            char share[32];
            if (sum == 0)
                std::snprintf(share, sizeof(share), "null");
            else
                std::snprintf(share, sizeof(share), "%.4f", static_cast<double>(row.bytes[b]) / static_cast<double>(sum));
            char piece[96];
            std::snprintf(piece, sizeof(piece), "\"%s\":%llu,\"%s_share\":%s", names[b],
                          static_cast<unsigned long long>(row.bytes[b]), names[b], share);
            out += piece;
        }
        out += "}}";
    }
    out += "]}\n";
    return out;
}

int AoiLeavePendingForViewer(uint64_t viewer_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (const auto &kv : g_leave_fail_since) {
        if (kv.first.viewer == viewer_id)
            ++n;
    }
    return n;
}

int AoiLeavePendingCount() {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<int>(g_leave_fail_since.size());
}

uint64_t AoiLeaveGivenUpCount() { return g_leave_give_up.load(std::memory_order_relaxed); }

uint64_t AoiLeaveDroppedOfflineCount() {
    return g_leave_dropped_offline.load(std::memory_order_relaxed);
}

void AoiResetEvidenceForTest() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_conn.clear();
        g_stall.clear();
        g_stalled_seen.clear();
        g_leave_fail_since.clear();
        g_move_frame_events.clear();
        g_move_frame_payload.clear();
        g_move_silence_ms.clear();
        g_visible_per_viewer.clear();
        g_leave_recover_ms.clear();
        g_busy_unacked.clear();
        g_busy_retrans_v.clear();
        g_busy_rto.clear();
        g_busy_cwnd.clear();
        g_busy_rtt.clear();
        g_busy_notsent.clear();
        g_busy_backoff.clear();
        g_busy_last_ack.clear();
        g_busy_last_sent.clear();
        g_busy_acked_delta.clear();
        g_busy_retrans_bytes_delta.clear();
        g_busy_total_retrans_delta.clear();
        MarkHotLocked();
    }
    for (int f = 0; f < static_cast<int>(AoiWriteFrame::Count); ++f) {
        g_tx[f].store(0, std::memory_order_relaxed);
        g_tx_frames[f].store(0, std::memory_order_relaxed);
        g_wire[f].store(0, std::memory_order_relaxed);
        g_egress_control[f].store(0, std::memory_order_relaxed);
        for (int r = 0; r < static_cast<int>(AoiWriteFailReason::Count); ++r)
            g_fail[f][r].store(0, std::memory_order_relaxed);
    }
    ServerStats::egress_budget_exhausted.store(0, std::memory_order_relaxed);
    ServerStats::egress_hard_silence_warn.store(0, std::memory_order_relaxed);
    ServerStats::egress_move_wire_bytes.store(0, std::memory_order_relaxed);
    ServerStats::egress_admitted_bytes.store(0, std::memory_order_relaxed);
    g_leave_fail.store(0, std::memory_order_relaxed);
    g_leave_recovered.store(0, std::memory_order_relaxed);
    g_leave_give_up.store(0, std::memory_order_relaxed);
    g_leave_dropped_offline.store(0, std::memory_order_relaxed);
    for (int i = 0; i < static_cast<int>(AoiLeaveDropReason::Count); ++i)
        g_leave_drop[i].store(0, std::memory_order_relaxed);
    g_leave_inconsistent.store(0, std::memory_order_relaxed);
    g_unsent_fn = nullptr;
    g_busy_samples.store(0, std::memory_order_relaxed);
    g_busy_retrans.store(0, std::memory_order_relaxed);
    g_busy_cwnd_low.store(0, std::memory_order_relaxed);
    g_busy_quiet.store(0, std::memory_order_relaxed);
    g_busy_with_enter.store(0, std::memory_order_relaxed);
    g_busy_with_snapshot.store(0, std::memory_order_relaxed);
    g_busy_tcp_ok.store(0, std::memory_order_relaxed);
    g_stalled_total.store(0, std::memory_order_relaxed);
    g_now_override.store(0, std::memory_order_relaxed);
}
