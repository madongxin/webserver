/**
 * @file GameTcpGateway.cpp
 * @brief 网关：拆帧投递 + Login/Reconnect 本地绑定 + 断线 MarkDisconnected
 */

#include "GameTcpGateway.h"

#include "AoiMoveCoalesceLedger.h"
#include "AoiStrip.h"
#include "InboundFrameBatch.h"
#include "MoveSendConfig.h"
#include "SendBufV2.h"
#include "SlowClientPolicy.h"
#include "AoiPublishQueue.h"
#include "Buffer.h"
#include "RecentSamples.h"
#include "ServerStats.h"
#include "CommandPolicy.h"
#include "ControlResponseOutbox.h"
#include "EventLoop.h"
#include "FormalMode.h"
#include "GameRequestPlayerId.h"
#include "GameRequestTransport.h"
#include "GatewayAuthFlow.h"
#include "GatewayAuthPolicy.h"
#include "GatewayConnGuard.h"
#include "GatewayConnRegistry.h"
#include "GatewayDisconnectAsync.h"
#include "GatewayIdentity.h"
#include "GatewayLoginRoute.h"
#include "HealthProbe.h"
#include "InProcessTransport.h"
#include "Logging.h"
#include "PacketTiming.h"
#include "LoginAdmission.h"
#include "MapCatalog.h"
#include "OpsMetrics.h"
#include "PlayerSerialQueue.h"
#include "ProtocolHandshake.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "ReliableOutbox.h"
#include "ReplySink.h"
#include "ReplyStage.h"

#include <sys/ioctl.h>
#include "RpcOffloadPool.h"
#include "SceneKind.h"
#include "ServiceHealth.h"
#include "SessionHandle.h"
#include "TcpConnection.h"
#include "TcpReplySink.h"
#include "TcpServer.h"
#include "game.pb.h"
#ifdef WEBSERVER_ENABLE_BRPC
#include <atomic>
#include <bthread/countdown_event.h>
#include <butil/time.h>
#endif

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#include "SessionStore.h"
#ifdef WEBSERVER_ENABLE_BRPC
#include "GatewayAuthClients.h"
#include "GatewayEnterMapOrchestrator.h"
#include "GatewayLoginOrchestrator.h"
#include "GatewayLogoutPolicy.h"
#include "SessionRpcClient.h"
#endif
#endif

#include <algorithm>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct ConnBind {
    uint64_t player_id = 0;
    std::string token;
    std::string session_id;
    uint64_t generation = 0;
};

std::mutex g_bind_mu;
std::unordered_map<uint64_t, ConnBind> g_conn_bind;  // connection_id -> bind

// 一条连接上的移动推送只留每个实体的最新位置，并且最多每 200ms 写出一帧。
// 否则 100 人挤在同一视野时，每次移动都会往这条连接的事件循环里排一个发送回调，
// 心跳和重连回包排在这些回调后面，要等整段队列跑完才能写到 socket。
struct MoveCoalesceSlot {
    std::mutex mu;
    uint64_t map_instance_id = 0;
    std::unordered_map<uint64_t, game::AoiEvent> by_entity;
    AoiMoveCoalesceLedger ledger;
    int64_t hold_since_ms = 0;
    bool scheduled = false;
    bool commit_hooked = false;
    bool viewer_known = false;
    float viewer_x = 0;
    float viewer_z = 0;
    SlowClientTracker slow;
    std::vector<game::AoiEvent> resync;
    int64_t last_park_swap_ms = 0;
};

std::mutex g_move_slots_mu;
std::unordered_map<uint64_t, std::shared_ptr<MoveCoalesceSlot>> g_move_slots;

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::mutex g_live_age_mu;
std::unordered_map<uint64_t, uint64_t> g_pending_age;
std::unordered_map<uint64_t, uint64_t> g_hold_age;

void RecomputeLiveAgeLocked(const std::unordered_map<uint64_t, uint64_t> &bucket,
                            std::atomic<uint64_t> &gauge, int64_t now) {
    uint64_t mx = 0;
    for (const auto &kv : bucket) {
        if (kv.second == 0 || now <= static_cast<int64_t>(kv.second))
            continue;
        const uint64_t age = static_cast<uint64_t>(now - static_cast<int64_t>(kv.second));
        if (age > mx)
            mx = age;
    }
    gauge.store(mx, std::memory_order_relaxed);
}

void PublishMaxAge(std::unordered_map<uint64_t, uint64_t> &bucket, std::atomic<uint64_t> &gauge,
                   uint64_t conn_id, uint64_t age) {
    const int64_t now = SteadyMs();
    if (age == 0)
        bucket.erase(conn_id);
    else
        bucket[conn_id] = static_cast<uint64_t>(now - static_cast<int64_t>(age));
    RecomputeLiveAgeLocked(bucket, gauge, now);
}

void RefreshLiveAges() {
    std::lock_guard<std::mutex> lk(g_live_age_mu);
    const int64_t now = SteadyMs();
    RecomputeLiveAgeLocked(g_pending_age, ServerStats::aoi_move_pending_oldest_ms, now);
    RecomputeLiveAgeLocked(g_hold_age, ServerStats::latest_move_hold_ms, now);
    ServerStats::aoi_move_entity_max_silence_ms.store(
        ServerStats::aoi_move_pending_oldest_ms.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

void PublishPendingAge(uint64_t conn_id, uint64_t age) {
    std::lock_guard<std::mutex> lk(g_live_age_mu);
    PublishMaxAge(g_pending_age, ServerStats::aoi_move_pending_oldest_ms, conn_id, age);
    // 静默看的是还没写出的位置等了多久。没有待发位置时必须回到 0，不能留着进程启动后的最大值。
    ServerStats::aoi_move_entity_max_silence_ms.store(
        ServerStats::aoi_move_pending_oldest_ms.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
}

void PublishHoldAge(uint64_t conn_id, uint64_t age) {
    std::lock_guard<std::mutex> lk(g_live_age_mu);
    PublishMaxAge(g_hold_age, ServerStats::latest_move_hold_ms, conn_id, age);
}

void ObservePendingMoves(const MoveCoalesceSlot &slot, uint64_t conn_id, int64_t now) {
    const int64_t oldest = slot.ledger.MaxSilenceMs(now);
    PublishPendingAge(conn_id, oldest > 0 ? static_cast<uint64_t>(oldest) : 0);
}

int MoveLayer(MoveCoalesceSlot *slot, uint64_t viewer, const game::AoiEvent &ev) {
    const auto &ent = ev.entity();
    if (viewer != 0 && ent.player_id() == viewer) {
        slot->viewer_known = true;
        slot->viewer_x = ent.position().x();
        slot->viewer_z = ent.position().z();
        return 1;
    }
    // 兴趣内的人同一档。分层打开时，32 米外是远层。
    if (AoiV2Layers() && viewer != 0 && slot->viewer_known && ent.has_position()) {
        const float dx = ent.position().x() - slot->viewer_x;
        const float dz = ent.position().z() - slot->viewer_z;
        const float near_m = static_cast<float>(AoiV2NearMeters());
        if (dx * dx + dz * dz > near_m * near_m)
            return 0;
    }
    return 1;
}

bool IsMoveOnlyAoi(const game::GameResponse &gr) {
    if (!gr.has_aoi_delta() || gr.aoi_delta().events_size() <= 0)
        return false;
    for (const auto &ev : gr.aoi_delta().events()) {
        if (ev.op() != 2)
            return false;
    }
    return true;
}

void EraseMoveSlot(uint64_t conn_id) {
    {
        std::lock_guard<std::mutex> lk(g_move_slots_mu);
        g_move_slots.erase(conn_id);
    }
    PublishPendingAge(conn_id, 0);
    PublishHoldAge(conn_id, 0);
}

void SampleConnSilence(std::vector<int64_t> *per_conn) {
    if (!per_conn)
        return;
    std::vector<std::shared_ptr<MoveCoalesceSlot>> slots;
    {
        std::lock_guard<std::mutex> lk(g_move_slots_mu);
        slots.reserve(g_move_slots.size());
        for (const auto &kv : g_move_slots)
            slots.push_back(kv.second);
    }
    const int64_t now = SteadyMs();
    per_conn->reserve(slots.size());
    for (const auto &slot : slots) {
        if (!slot)
            continue;
        std::lock_guard<std::mutex> lk(slot->mu);
        const int64_t age = slot->ledger.MaxSilenceMs(now);
        if (age > 0)
            per_conn->push_back(age);
    }
}

void SampleMoveSilence(std::vector<int64_t> *near, std::vector<int64_t> *far) {
    std::vector<std::shared_ptr<MoveCoalesceSlot>> slots;
    {
        std::lock_guard<std::mutex> lk(g_move_slots_mu);
        slots.reserve(g_move_slots.size());
        for (const auto &kv : g_move_slots)
            slots.push_back(kv.second);
    }
    const int64_t now = SteadyMs();
    for (const auto &slot : slots) {
        if (!slot)
            continue;
        std::lock_guard<std::mutex> lk(slot->mu);
        slot->ledger.AppendSilence(now, near, far);
    }
}

struct RegisterMoveSilence {
    RegisterMoveSilence() {
        OpsMetrics::SetAoiSilenceSampler(&SampleMoveSilence);
        OpsMetrics::SetConnSilenceSampler(&SampleConnSilence);
    }
};
RegisterMoveSilence g_register_move_silence;

std::shared_ptr<MoveCoalesceSlot> FindMoveSlot(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_move_slots_mu);
    auto it = g_move_slots.find(conn_id);
    if (it == g_move_slots.end())
        return nullptr;
    return it->second;
}

void DiscardCoalescedAoiMoves(uint64_t conn_id) { EraseMoveSlot(conn_id); }

struct RegisterMoveDiscard {
    RegisterMoveDiscard() {
        ControlResponseOutbox::SetPendingMoveDiscard(&DiscardCoalescedAoiMoves);
    }
};
RegisterMoveDiscard g_register_move_discard;

void FlushMoveSlot(const std::shared_ptr<MoveCoalesceSlot> &slot,
                   const std::weak_ptr<TcpConnection> &weak, uint64_t conn_id);

void ApplyMoveCommit(const std::shared_ptr<MoveCoalesceSlot> &slot,
                     const std::shared_ptr<TcpConnection> &live,
                     std::vector<TcpConnection::LatestMoveAck> ents, bool committed, int frame_bytes) {
    ServerStats::aoi_move_commit_callbacks.fetch_add(1, std::memory_order_relaxed);
    bool again = false;
    const uint64_t conn_id = live ? live->id() : 0;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        const int64_t now = SteadyMs();
        std::vector<AoiMoveStamp> stamps;
        stamps.reserve(ents.size());
        for (const auto &e : ents) {
            if (e.entity_id == 0)
                continue;
            AoiMoveStamp stamp;
            stamp.entity_id = e.entity_id;
            stamp.state_seq = e.state_seq;
            stamps.push_back(stamp);
        }
        if (committed) {
            slot->ledger.NoteKernelWrite(stamps, now, frame_bytes);
            slot->slow.NoteMoveWritten(now);
            if (frame_bytes > 0) {
                OpsMetrics::Instance().AddGatewayTxBytes(static_cast<uint64_t>(frame_bytes));
                OpsMetrics::Instance().AddGatewayTxFrames(1);
            }
            for (const auto &e : ents) {
                auto it = slot->by_entity.find(e.entity_id);
                if (it == slot->by_entity.end())
                    continue;
                if (it->second.entity().state_seq() == e.state_seq)
                    slot->by_entity.erase(it);
            }
            slot->hold_since_ms = 0;
            slot->last_park_swap_ms = 0;
        } else if (frame_bytes < 0) {
            slot->ledger.NoteParkSwapped(stamps);
            slot->hold_since_ms = 0;
        } else {
            slot->ledger.NoteReplaced(stamps);
            slot->hold_since_ms = 0;
        }
        ObservePendingMoves(*slot, conn_id, now);
        again = !slot->by_entity.empty();
        slot->scheduled = again;
    }
    PublishHoldAge(conn_id, 0);
    if (!again || !live || !live->loop())
        return;
    std::weak_ptr<TcpConnection> weak = live;
    live->loop()->RunAfter(0.0, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
}

void HookMoveCommit(const std::shared_ptr<TcpConnection> &c,
                    const std::shared_ptr<MoveCoalesceSlot> &slot) {
    if (!c || !slot || slot->commit_hooked)
        return;
    slot->commit_hooked = true;
    std::weak_ptr<TcpConnection> weak = c;
    std::weak_ptr<MoveCoalesceSlot> wslot = slot;
    c->set_latest_move_commit_callback(
        [weak, wslot](std::vector<TcpConnection::LatestMoveAck> ents, bool committed, int bytes) {
            auto live = weak.lock();
            auto held = wslot.lock();
            if (!held)
                return;
            ApplyMoveCommit(held, live, std::move(ents), committed, bytes);
        });
}

void FlushMoveSlot(const std::shared_ptr<MoveCoalesceSlot> &slot,
                   const std::weak_ptr<TcpConnection> &weak, uint64_t conn_id) {
    auto live = weak.lock();
    if (!slot || !live || live->state() != TcpConnection::Connected || !live->loop()) {
        if (slot) {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->by_entity.clear();
            slot->scheduled = false;
        }
        EraseMoveSlot(conn_id);
        return;
    }
    HookMoveCommit(live, slot);
    // 半帧已经进了内核就等写完。还没写出第一个字节的停放帧可以换成更新的一批。
    if (live->latest_move_awaiting_write()) {
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->slow.NoteServerDeferred(SteadyMs());
        }
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    if (live->holding_latest_move()) {
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            slot->slow.NoteServerDeferred(SteadyMs());
        }
        live->NoteParkSample();
        if (live->move_parked_on_kernel_queue()) {
            // 队列还高于 16KB 时不写这一帧，也不每 200ms 记一次失败。
            // 满 1 秒才换成槽里的最新坐标，失败次数和等待起点都不变。
            bool swap = false;
            {
                std::lock_guard<std::mutex> lk(slot->mu);
                const int64_t now = SteadyMs();
                if (slot->last_park_swap_ms == 0)
                    slot->last_park_swap_ms = now;
                else if (now - slot->last_park_swap_ms >= 1000) {
                    slot->last_park_swap_ms = now;
                    swap = true;
                }
            }
            if (!swap) {
                live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
                return;
            }
            live->ReleaseParkedMoveForSwap();
            if (live->holding_latest_move() || live->latest_move_awaiting_write()) {
                live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
                return;
            }
        } else {
            // 队列已经降到 16KB。定时器负责写出，这里不能把还没进内核的帧丢掉。
            live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
            return;
        }
    }
    constexpr int kMaxMoveEntities = 200;
    int64_t wait_ms = 0;
    bool leftover = false;
    bool close_for_slow = false;
    game::AoiDelta delta;
    std::vector<AoiMoveStamp> picked;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        const int64_t now = SteadyMs();
        const bool overflow =
            ControlResponseOutbox::Instance().Depth(conn_id) >= ControlResponseOutbox::kMaxFrames;
        const SlowClientAction slow_action = slot->slow.Evaluate(now, overflow);
        if (AoiV2Layers())
            slot->ledger.SetFarIntervalScale(slot->slow.degraded() ? 2 : 1);
        if (slow_action == SlowClientAction::Degrade) {
            if (!AoiV2Layers()) {
                slot->ledger.SetQuotaPercent(50);
                slot->ledger.SetSendIntervalMs(1000);
            }
            ServerStats::aoi_move_slow_degrade.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO << "slow client degrade conn#" << conn_id;
        } else if (slow_action == SlowClientAction::ClearSlot) {
            slot->resync.clear();
            for (const auto &kv : slot->by_entity)
                slot->resync.push_back(kv.second);
            slot->by_entity.clear();
            slot->ledger.ClearPending();
            ServerStats::aoi_move_slow_clear.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO << "slow client clear slot conn#" << conn_id << " entities=" << slot->resync.size();
        } else if (slow_action == SlowClientAction::Close) {
            ServerStats::aoi_move_slow_close.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN << "slow client close conn#" << conn_id;
            close_for_slow = true;
        }
        if (slot->slow.snapshot_due() && !slot->resync.empty()) {
            if (AoiV2VisibleSet()) {
                slot->resync.clear();
                slot->slow.NoteSnapshotSent();
            } else {
            for (const auto &ev : slot->resync) {
                const uint64_t id = ev.entity().player_id();
                if (id == 0)
                    continue;
                slot->by_entity[id] = ev;
                slot->ledger.Offer(id, ev.entity().state_seq(), now, 1);
            }
            slot->resync.clear();
            slot->slow.NoteSnapshotSent();
            ServerStats::aoi_move_snapshot_resync.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (slot->by_entity.empty()) {
            slot->scheduled = false;
        } else if (ReliableOutboxHub::Instance().Depth(conn_id) > 0 ||
                   ControlResponseOutbox::Instance().BlocksMove(conn_id)) {
            ObservePendingMoves(*slot, conn_id, now);
            wait_ms = 200;
        } else {
            int64_t conns = GatewayConnRegistry::Instance().BoundPlayerCount();
            if (conns < 1)
                conns = OpsMetrics::Instance().gateway_tcp_connections();
            if (conns < 1)
                conns = 1;
            const int per = std::max(
                1, AoiMoveCoalesceLedger::EgressBudgetBytesPerSec() / static_cast<int>(conns));
            const int event_bytes =
                AoiExtrapolateEnabled() ? kAoiMoveExtrapEventBytes : kAoiMoveEventBytes;
            slot->ledger.SetEnforceByteBudget(AoiV2Layers());
            picked = slot->ledger.Pick(now, per, event_bytes, kMaxMoveEntities, &wait_ms);
            if (!picked.empty()) {
                delta.set_map_instance_id(slot->map_instance_id);
                for (const auto &stamp : picked) {
                    auto it = slot->by_entity.find(stamp.entity_id);
                    if (it == slot->by_entity.end())
                        continue;
                    *delta.add_events() = it->second;
                }
                slot->ledger.NoteParked(picked);
                slot->scheduled = !slot->by_entity.empty();
            }
        }
        if (wait_ms > 0)
            slot->slow.NoteServerDeferred(now);
    }
    if (close_for_slow) {
        EraseMoveSlot(conn_id);
        live->CloseForSlowClient("slow_client");
        return;
    }
    if (wait_ms > 0) {
        live->loop()->RunAfter(static_cast<double>(wait_ms) / 1000.0,
                               [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    if (delta.events_size() == 0) {
        bool pending = false;
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            pending = !slot->by_entity.empty();
            if (pending)
                slot->scheduled = true;
        }
        if (pending)
            live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    const int event_n = delta.events_size();
    auto give_back = [&]() {
        std::lock_guard<std::mutex> lk(slot->mu);
        slot->ledger.NoteReplaced(picked);
        if (!slot->scheduled && !slot->by_entity.empty())
            slot->scheduled = true;
    };
    // 回包或可靠帧还在等发送时，这一帧不能进内核。人留在槽里，等待时间不重置。
    if (ReliableOutboxHub::Instance().Depth(conn_id) > 0 ||
        ControlResponseOutbox::Instance().BlocksMove(conn_id)) {
        give_back();
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    // 还停在应用缓冲、定时器没接手的旧帧可以换成更新的位置。
    const size_t overwritten = DropQueuedAoiMoveFrames(live.get(), false);
    if (overwritten != 0)
        ServerStats::aoi_move_overwritten.fetch_add(overwritten, std::memory_order_relaxed);
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_message("aoi");
    *notify.mutable_aoi_delta() = delta;
    std::string body;
    std::string frame;
    if (!notify.SerializeToString(&body) || !gameproto::EncodeFrame(body, &frame)) {
        give_back();
        NoteMoveFlush(false, event_n);
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    std::vector<TcpConnection::LatestMoveAck> acks;
    acks.reserve(picked.size());
    for (const auto &stamp : picked) {
        TcpConnection::LatestMoveAck ack;
        ack.entity_id = stamp.entity_id;
        ack.state_seq = stamp.state_seq;
        acks.push_back(ack);
    }
    const bool sent = live->TrySendLatestMove(frame, acks);
    const bool held = sent && live->holding_latest_move();
    if (live->send_buf())
        NoteSendPending(live->send_buf()->readablebytes(), live->unsent_bytes());
    if (!sent) {
        NoteMoveFlush(false, event_n);
        ServerStats::aoi_move_buffer_hold.fetch_add(1, std::memory_order_relaxed);
        // 64KB 拒绝不会回调。写失败的回调已经把人放回候选集，这里再调一次是空操作的前提是
        // NoteReplaced 对已经不在途的人不再加惩罚。为了测试里「未停放也要惩罚」，这里只在仍 in_flight 时归还。
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            std::vector<AoiMoveStamp> still;
            for (const auto &stamp : picked) {
                if (slot->ledger.IsInFlight(stamp.entity_id))
                    still.push_back(stamp);
            }
            if (!still.empty())
                slot->ledger.NoteReplaced(still);
        }
        NoteMovePendingEntities(event_n);
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    if (held) {
        NoteMoveHold(event_n);
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            const int64_t now = SteadyMs();
            if (slot->hold_since_ms == 0)
                slot->hold_since_ms = now;
            const int64_t age = now - slot->hold_since_ms;
            PublishHoldAge(conn_id, age > 0 ? static_cast<uint64_t>(age) : 0);
            ObservePendingMoves(*slot, conn_id, now);
            if (!slot->scheduled && !slot->by_entity.empty())
                slot->scheduled = true;
        }
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
        return;
    }
    NoteMoveFlush(true, event_n);
    int pending = 0;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        const int64_t now = SteadyMs();
        ObservePendingMoves(*slot, conn_id, now);
        pending = static_cast<int>(slot->by_entity.size());
        if (pending > 0 && !slot->scheduled)
            slot->scheduled = true;
        leftover = pending > 0;
    }
    NoteMovePendingEntities(pending);
    if (leftover)
        live->loop()->RunAfter(0.2, [slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
}

// 纯移动帧被合并后返回 true。ENTER/LEAVE 或解析不了的帧返回 false，调用方照旧立刻发送。
bool OfferCoalescedMove(const std::shared_ptr<TcpConnection> &c, const std::string &frame) {
    if (!c || !c->loop() || frame.empty())
        return false;
    std::string buf = frame;
    std::string payload;
    if (gameproto::DecodeOneFrame(&buf, &payload) != gameproto::FrameDecodeResult::Complete)
        return false;
    game::GameResponse gr;
    if (!gr.ParseFromString(payload) || !IsMoveOnlyAoi(gr))
        return false;
    std::shared_ptr<MoveCoalesceSlot> slot;
    {
        std::lock_guard<std::mutex> lk(g_move_slots_mu);
        auto &found = g_move_slots[c->id()];
        if (!found)
            found = std::make_shared<MoveCoalesceSlot>();
        slot = found;
    }
    uint64_t viewer = 0;
    {
        std::lock_guard<std::mutex> blk(g_bind_mu);
        auto bit = g_conn_bind.find(c->id());
        if (bit != g_conn_bind.end())
            viewer = bit->second.player_id;
    }
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lk(slot->mu);
        if (gr.aoi_delta().map_instance_id() != 0)
            slot->map_instance_id = gr.aoi_delta().map_instance_id();
        const int64_t now = SteadyMs();
        for (const auto &ev : gr.aoi_delta().events()) {
            const uint64_t id = ev.entity().player_id();
            if (id == 0)
                continue;
            auto &kept = slot->by_entity[id];
            // 序号不增加的移动客户端会整条丢掉。只保留更大的 state_seq。
            if (kept.has_entity() && ev.entity().state_seq() < kept.entity().state_seq())
                continue;
            kept = ev;
            slot->ledger.Offer(id, ev.entity().state_seq(), now, MoveLayer(slot.get(), viewer, ev));
        }
        ObservePendingMoves(*slot, c->id(), SteadyMs());
        if (slot->by_entity.empty())
            return true;
        if (!slot->scheduled) {
            slot->scheduled = true;
            schedule = true;
        }
        NoteMovePendingEntities(static_cast<int>(slot->by_entity.size()));
    }
    NoteMoveOffer(schedule);
    if (!schedule)
        return true;
    const uint64_t conn_id = c->id();
    std::weak_ptr<TcpConnection> weak = c;
    c->loop()->QueueOneFunc([slot, weak, conn_id]() { FlushMoveSlot(slot, weak, conn_id); });
    return true;
}
std::unordered_set<uint64_t> g_authoritative_logout;  // conn_id：主动 Logout 成功，断线不得 MarkDisconnected
std::string g_gateway_id = "gw-0";
std::atomic<bool> g_tcp_listening{false};

void RememberBind(uint64_t conn_id, uint64_t player_id, const std::string &token,
                  const std::string &session_id, uint64_t generation,
                  std::shared_ptr<ReplySink> sink,
                  const gameproto::GatewayLoginRoute *route = nullptr) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    ConnBind b;
    b.player_id = player_id;
    b.token = token;
    b.session_id = session_id;
    b.generation = generation;
    g_conn_bind[conn_id] = b;

    GatewayConnRegistry::Bind gb;
    gb.player_id = player_id;
    gb.token = token;
    gb.session_id = session_id;
    gb.generation = generation;
    gb.connection_id = conn_id;
    gb.gateway_instance_id = g_gateway_id;
    if (route) {
        gb.gamelogic_instance_id = route->gamelogic_instance_id;
        gb.map_instance_id = route->map_instance_id;
        gb.map_owner_epoch = route->map_owner_epoch;
        gb.route_version = route->route_version;
    }
        if (sink) {
        std::weak_ptr<TcpConnection> weak_conn = sink->tcp_connection();
        auto make_try_send = [weak_conn](bool urgent) {
            return [weak_conn, urgent](const std::string &frame) -> int {
                auto c = weak_conn.lock();
                if (!c || !c->loop())
                    return -1;
                // 推送路径用 TrySend：队列满丢帧，禁止背压关连接（否则 MarkDisconnected 雪崩）。
                // 1 = 进了应用缓冲或当场 write。不是内核写完，也不是客户端已应用。
                auto do_send = [weak_conn, frame, urgent]() -> int {
                    auto live = weak_conn.lock();
                    if (!live || live->state() != TcpConnection::Connected)
                        return -1;
                    bool ok = false;
                    if (urgent && SendBufV2Enabled()) {
                        const auto rc =
                            live->TrySendControl(frame, true, AoiStripEntityIds(frame));
                        ok = rc == TcpConnection::WrittenDirect || rc == TcpConnection::Buffered;
                    } else {
                        if (urgent)
                            MakeRoomForControl(live.get());
                        ok = urgent ? live->TrySendUrgent(frame) : live->TrySend(frame);
                    }
                    // ENTER/LEAVE 发不出去时丢掉排队的移动包再试一次。不改这一帧的内容。
                    if (!ok && urgent) {
                        const size_t dropped = DropQueuedAoiMoveFrames(live.get());
                        bool retried = false;
                        if (dropped != 0 && SendBufV2Enabled()) {
                            const auto rc =
                                live->TrySendControl(frame, true, AoiStripEntityIds(frame));
                            retried = rc == TcpConnection::WrittenDirect || rc == TcpConnection::Buffered;
                        } else if (dropped != 0) {
                            retried = live->TrySendUrgent(frame);
                        }
                        if (retried) {
                            LOG_INFO << "Gateway dropped move frames for aoi conn#" << live->id()
                                     << " moves=" << dropped << " bytes=" << frame.size();
                            ok = true;
                        }
                    }
                    if (!ok)
                        return 0;
                    OpsMetrics::Instance().AddGatewayTxBytes(frame.size());
                    OpsMetrics::Instance().AddGatewayTxFrames(1);
                    return 1;
                };
                if (c->loop()->IsInLoopThread())
                    return do_send();
#ifdef WEBSERVER_ENABLE_BRPC
                // 等的是入队结果，不是 TCP ACK。不能在连接自己的 EventLoop 上等，否则死锁。
                // 超时后不取消回调：序号已经写入回放，取消会在 TCP 仍存活时留下跳号。
                // 回调按入队顺序执行，后到的推送排在这一帧后面。
                auto done = std::make_shared<bthread::CountdownEvent>();
                auto queued = std::make_shared<std::atomic<int>>(0);
                c->loop()->QueueOneFunc([do_send, done, queued]() {
                    queued->store(do_send(), std::memory_order_release);
                    done->signal();
                });
                if (done->timed_wait(butil::milliseconds_from_now(200)) != 0) {
                    OpsMetrics::Instance().IncPushSendDeferred();
                    // 急送超时还没写进缓冲。返回 0，调用方继续把 LEAVE 留在差集里。
                    // 已经入队的回调仍会写；写失败时下一轮再发，重复的 LEAVE 可以再收一次。
                    if (urgent)
                        return 0;
                    return 1;
                }
                return queued->load(std::memory_order_acquire);
#else
                c->loop()->QueueOneFunc([do_send]() { (void)do_send(); });
                return 0;
#endif
            };
        };
        auto send_move_now = make_try_send(false);
        gb.send_frame = [weak_conn, send_move_now](const std::string &frame) -> int {
            auto c = weak_conn.lock();
            if (!c || !c->loop())
                return -1;
            if (OfferCoalescedMove(c, frame))
                return 1;
            return send_move_now(frame);
        };
        gb.send_frame_urgent = make_try_send(true);
        gb.send_urgent_raw = [weak_conn](const std::string &frame) -> int {
            auto live = weak_conn.lock();
            if (!live || live->state() != TcpConnection::Connected || !live->loop() ||
                !live->loop()->IsInLoopThread())
                return -1;
            // 回包队列不空时不能直接放弃。MOVE 可以等回包，ENTER/LEAVE 不行：
            // 观察者名单已经删了，这一帧写不出去客户端就一直留着旧模型。
            MakeRoomForControl(live.get());
            // ENTER/LEAVE 走控制发送流，插在 MOVE 前面。32KB 的紧急水位会在回包已经占满缓冲时一直失败，
            // 可靠队列不空又会禁止 AOI MOVE，位置更新就会停住。
            const auto strip = AoiStripEntityIds(frame);
            auto rc = live->TrySendControl(frame, true, strip);
            if (rc == TcpConnection::Blocked) {
                const size_t dropped = DropQueuedAoiMoveFrames(live.get());
                if (dropped != 0)
                    rc = live->TrySendControl(frame, true, strip);
            }
            if (rc == TcpConnection::WrittenDirect || rc == TcpConnection::Buffered) {
                OpsMetrics::Instance().AddGatewayTxBytes(frame.size());
                OpsMetrics::Instance().AddGatewayTxFrames(1);
                return 1;
            }
            const int app = live->send_buf() ? live->send_buf()->readablebytes() : 0;
            const int unsent = live->unsent_bytes();
            const int pending = app + (unsent > 0 ? unsent : 0);
            OpsMetrics::Instance().NoteReliableSendFailBytes(
                static_cast<uint64_t>(pending > 0 ? pending : 0));
            return rc == TcpConnection::Closed || live->state() != TcpConnection::Connected ? -1 : 0;
        };
        if (auto live = weak_conn.lock()) {
            live->set_drain_callback([weak_conn, conn_id]() {
                auto c = weak_conn.lock();
                if (c) {
                    if (auto slot = FindMoveSlot(conn_id)) {
                        std::lock_guard<std::mutex> lk(slot->mu);
                        slot->slow.NoteAnyByteWritten(SteadyMs());
                    }
                }
                // 已进入发送流的控制帧不挡住队列里还没写进去的下一条。
                // ENTER/LEAVE 跟在回包后面继续写，不能等回包队列清空。
                ControlResponseOutbox::Instance().OnBufferDrained(conn_id);
                ReliableOutboxHub::Instance().OnBufferDrained(conn_id);
            });
        }
        gb.close_conn = [weak_conn]() {
            auto c = weak_conn.lock();
            if (!c)
                return;
            TcpReplySink live(c);
            live.CloseConnection();
        };
        gb.run_after = [weak_conn](double delay_sec, std::function<void()> cb) {
            auto c = weak_conn.lock();
            if (!c) {
                if (cb)
                    cb();
                return;
            }
            c->loop()->RunAfter(delay_sec, [weak_conn, cb = std::move(cb)]() {
                auto live = weak_conn.lock();
                if (!live)
                    return;
                if (cb)
                    cb();
            });
        };
        gb.queue_on_loop = [weak_conn](std::function<void()> fn) {
            auto c = weak_conn.lock();
            if (!c || !c->loop()) {
                if (fn)
                    fn();
                return;
            }
            c->loop()->QueueOneFunc(std::move(fn));
        };
    }
    GatewayConnRegistry::Instance().Remember(conn_id, std::move(gb));
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

void ForgetBind(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
    ReliableOutboxHub::Instance().Forget(conn_id);
    ControlResponseOutbox::Instance().Forget(conn_id);
    EraseMoveSlot(conn_id);
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

void MarkAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_authoritative_logout.insert(conn_id);
    g_conn_bind.erase(conn_id);
    GatewayConnRegistry::Instance().Forget(conn_id);
    ReliableOutboxHub::Instance().Forget(conn_id);
    ControlResponseOutbox::Instance().Forget(conn_id);
    EraseMoveSlot(conn_id);
    OpsMetrics::Instance().SetOnlinePlayers(GatewayConnRegistry::Instance().BoundPlayerCount());
}

bool ConsumeAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_authoritative_logout.erase(conn_id) > 0;
}

bool HasAuthoritativeLogout(uint64_t conn_id) {
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_authoritative_logout.count(conn_id) > 0;
}

void SendPublicErr(const std::shared_ptr<ReplySink> &sink, uint64_t conn_id, uint64_t seq,
                   const char *code, const char *msg) {
    std::string out;
    if (gameproto::EncodePublicErrorFrame(code, msg, seq, conn_id, &out) && sink)
        sink->SendFrame(out);
}

void ScheduleIdleCheck(std::weak_ptr<TcpConnection> weak, uint64_t id) {
    auto conn = weak.lock();
    if (!conn)
        return;
    conn->loop()->RunAfter(0.5, [weak, id]() {
        auto c = weak.lock();
        if (!c)
            return;
        const bool hello_ok = GatewayConnGuard::Instance().HelloOk(id);
        uint32_t idle_ms = gameproto::IdleTimeoutMs();
        if (!hello_ok && ClientHelloRequired())
            idle_ms = gameproto::HelloDeadlineMs();
        // 正式环境已绑定连接的空闲超时是 1 小时。没绑上玩家的重连套接字不能跟着留着，
        // 否则 TCP 数会高于在线人数。20 秒够 Hello 和 Login，登录本身是几百毫秒。
        bool bound = false;
        {
            std::lock_guard<std::mutex> lk(g_bind_mu);
            auto bit = g_conn_bind.find(id);
            bound = bit != g_conn_bind.end() && bit->second.player_id != 0;
        }
        if (!bound && GatewayConnGuard::Instance().ConnectedAgeMs(id) >= 20000) {
            OpsMetrics::Instance().IncIdleTimeout();
            LOG_WARN << "Gateway unbound timeout conn#" << id << " hello_ok=" << hello_ok;
            c->HandleClose();
            return;
        }
        if (idle_ms < 200)
            idle_ms = 200;
        int unread = 0;
        if (c->fd() >= 0)
            ::ioctl(c->fd(), FIONREAD, &unread);
        if (unread > 0 || (c->read_buf() && c->read_buf()->readablebytes() > 0)) {
            GatewayConnGuard::Instance().NoteActivity(id, static_cast<size_t>(unread > 0 ? unread : 1));
            ScheduleIdleCheck(weak, id);
            return;
        }
        if (GatewayConnGuard::Instance().IdleExpired(id, idle_ms)) {
            OpsMetrics::Instance().IncIdleTimeout();
            LOG_WARN << "Gateway idle timeout conn#" << id << " hello_ok=" << hello_ok;
            c->HandleClose();
            return;
        }
        ScheduleIdleCheck(weak, id);
    });
}

class BindingReplySink : public ReplySink {
public:
    BindingReplySink(std::shared_ptr<ReplySink> inner, uint64_t conn_id, uint64_t player_id,
                     bool is_login_or_reconnect)
        : inner_(std::move(inner)),
          conn_id_(conn_id),
          player_id_(player_id),
          bindable_(is_login_or_reconnect) {}

    void SendFrame(const std::string &response_frame) override {
        if (bindable_ && player_id_ != 0) {
            std::string buf = response_frame;
            std::string payload;
            if (gameproto::DecodeOneFrame(&buf, &payload) == gameproto::FrameDecodeResult::Complete) {
                game::GameResponse rsp;
                if (rsp.ParseFromString(payload) && rsp.ok()) {
                    std::string token;
                    std::string session_id;
                    uint64_t generation = 0;
                    if (rsp.has_login() && rsp.login().ok()) {
                        token = rsp.login().token();
                        session_id = rsp.login().session_id();
                        generation = rsp.login().generation();
                    } else if (rsp.has_reconnect() && rsp.reconnect().ok()) {
                        token = rsp.reconnect().token();
                        session_id = rsp.reconnect().session_id();
                        generation = rsp.reconnect().generation();
                    }
                    if (!token.empty()) {
                        RememberBind(conn_id_, player_id_, token, session_id, generation, inner_);
#ifdef WEBSERVER_ENABLE_REDIS
#ifdef WEBSERVER_ENABLE_BRPC
                        if (SessionRpcClient::Instance().ready()) {
                            SessionRpcClient::Instance().BindConnection(player_id_, token,
                                                                       g_gateway_id, conn_id_);
                        } else
#endif
                            if (SessionStore::Instance().Available()) {
                            SessionStore::Instance().BindConnection(player_id_, token, g_gateway_id,
                                                                   conn_id_);
                        }
#endif
                        LOG_INFO << "Gateway bind conn#" << conn_id_ << " player_id=" << player_id_
                                 << " generation=" << generation;
                    }
                }
            }
        }
        if (inner_)
            inner_->SendFrame(response_frame);
    }

    std::shared_ptr<TcpConnection> tcp_connection() const override {
        return inner_ ? inner_->tcp_connection() : nullptr;
    }

private:
    std::shared_ptr<ReplySink> inner_;
    uint64_t conn_id_ = 0;
    uint64_t player_id_ = 0;
    bool bindable_ = false;
};

}  // namespace

GameTcpGateway::GameTcpGateway(const std::string &ip, int port, const std::string &instance_id)
    : ip_(ip), port_(port), instance_id_(instance_id) {
    if (instance_id_.empty() && GatewayIdentity::Instance().ready())
        instance_id_ = GatewayIdentity::Instance().id();
    if (instance_id_.empty())
        instance_id_ = "gw-0";
    g_gateway_id = instance_id_;
}

bool GameTcpGateway::TcpListening() { return g_tcp_listening.load(std::memory_order_acquire); }

GameTcpGateway::~GameTcpGateway() {
    RequestQuit();
    if (thread_.joinable())
        thread_.join();
}

void GameTcpGateway::StartInBackground() {
    thread_ = std::thread([this]() { Run(); });
}

void GameTcpGateway::StopAccepting() {
    EventLoop *lp = loop_.load();
    TcpServer *srv = server_.load();
    if (!lp || !srv)
        return;
    lp->QueueOneFunc([srv]() { srv->StopAccepting(); });
}

void GameTcpGateway::RequestQuit() {
    EventLoop *lp = loop_.load();
    if (!lp)
        return;
    lp->QueueOneFunc([lp]() { lp->Quit(); });
}

void GameTcpGateway::Run() {
    InProcessTransport::Instance().EnsureStarted(0);
    RpcOffloadPool::Instance().Start(0);
#ifdef WEBSERVER_ENABLE_REDIS
    GatewayDisconnectAsync::Instance().Start(4096);
#endif

    EventLoop loop;
    auto server = std::make_unique<TcpServer>(&loop, ip_.c_str(), port_);
    loop_.store(&loop);
    server_.store(server.get());
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers <= 1)
        workers = 1;
    else
        --workers;
    server->SetThreadNums(workers);
    server->set_connection_callback([](const std::shared_ptr<TcpConnection> &c) {
        OpsMetrics::Instance().IncTcpConnect();
        if (ServiceHealth::Instance().draining()) {
            OpsMetrics::Instance().IncDrainReject();
            c->HandleClose();
            return;
        }
        const std::string lim = GatewayConnGuard::Instance().CheckConnectRate(c->fd());
        if (!lim.empty()) {
            OpsMetrics::Instance().IncConnRateLimited();
            LOG_WARN << "Gateway connect rate-limited conn#" << c->id();
            c->HandleClose();
            return;
        }
        GatewayConnGuard::Instance().OnConnected(c->id(), c->fd());
        GatewayAuthFlow::Instance().OnConnected(c->id());
        (void)ConsumeAuthoritativeLogout(c->id());
        std::weak_ptr<TcpConnection> weak = c;
        const uint64_t id = c->id();
        c->loop()->QueueOneFunc([weak, id]() { ScheduleIdleCheck(weak, id); });
    });
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    server->set_disconnect_callback([](const std::shared_ptr<TcpConnection> &c) {
        OpsMetrics::Instance().IncTcpDisconnect();
        GatewayAuthFlow::Instance().OnDisconnected(c->id());
        GatewayConnGuard::Instance().OnDisconnected(c->id());
        ConnBind bind;
        {
            std::lock_guard<std::mutex> lk(g_bind_mu);
            auto it = g_conn_bind.find(c->id());
            if (it != g_conn_bind.end()) {
                bind = it->second;
                g_conn_bind.erase(it);
            }
        }
#ifdef WEBSERVER_ENABLE_BRPC
        GatewayConnRegistry::Bind reg;
        const bool taken_over =
            GatewayConnRegistry::Instance().HasNewerBinding(c->id(), bind.player_id);
        const bool has_reg =
            !taken_over && GatewayConnRegistry::Instance().FindByConnection(c->id(), &reg);
#else
        const bool taken_over =
            GatewayConnRegistry::Instance().HasNewerBinding(c->id(), bind.player_id);
#endif
        // 必须在 g_bind_mu 之外 Forget：ForgetBind 会再锁同一把非递归 mutex。
        // 未绑定连接（Register 超时后客户端断开）走旧 early-return 会把 acceptor EventLoop 卡死，
        // listen backlog 堆满后所有新登录 no_response。
        ForgetBind(c->id());

        const bool logout_done = ConsumeAuthoritativeLogout(c->id());
        if (logout_done) {
            // 主动 Logout 已释放 Session；断线不得把 Session 写回 DISCONNECTED。
            return;
        }

        if (taken_over) {
            LOG_INFO << "Gateway disconnect stale conn#" << c->id() << " player_id="
                     << bind.player_id << " generation=" << bind.generation
                     << " (player moved to a newer conn; skip MarkDisconnected/Unbind)";
            return;
        }

        if (bind.player_id != 0) {
#ifdef WEBSERVER_ENABLE_REDIS
#ifdef WEBSERVER_ENABLE_BRPC
            if (SessionRpcClient::Instance().ready()) {
                SessionRpcClient::Instance().MarkDisconnectedAsync(bind.player_id, bind.token,
                                                                   bind.generation);
                OpsMetrics::Instance().IncDisconnectAccepted();
            } else
#endif
                if (SessionStore::Instance().Available()) {
                // 本地 Redis 兜底：后台队列，禁止在 Reactor 同步访问 Redis
                if (GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(
                        bind.player_id, bind.token, bind.generation)) {
                    OpsMetrics::Instance().IncDisconnectAccepted();
                } else {
#ifdef WEBSERVER_ENABLE_BRPC
                    if (SessionRpcClient::Instance().ready()) {
                        SessionRpcClient::Instance().MarkDisconnectedAsync(
                            bind.player_id, bind.token, bind.generation);
                        OpsMetrics::Instance().IncDisconnectRetried();
                        OpsMetrics::Instance().IncDisconnectAccepted();
                        LOG_WARN << "disconnect queue full, rpc fallback player=" << bind.player_id;
                    } else
#endif
                    {
                        OpsMetrics::Instance().IncDisconnectDropped();
                        OpsMetrics::Instance().IncDisconnectFailed();
                        LOG_WARN << "disconnect redis enqueue failed player=" << bind.player_id;
                    }
                }
            }
#endif
#ifdef WEBSERVER_ENABLE_BRPC
            if (has_reg && !reg.gamelogic_instance_id.empty() &&
                GatewayAuthClients::Instance().ready()) {
                glrpc::UnbindPlayerRequest ureq;
                ureq.set_player_id(bind.player_id);
                ureq.set_session_id(bind.session_id);
                ureq.set_fence_token(bind.token);
                ureq.set_generation(bind.generation);
                ureq.set_reason("tcp_disconnect");
                GatewayAuthClients::Instance().UnbindPlayerAsync(reg.gamelogic_instance_id, ureq);
            }
#endif
            game::GameRequest flush_req;
            flush_req.set_seq(0);
            flush_req.set_session_token(bind.token);
            auto *fb = flush_req.mutable_flush_bag();
            fb->set_player_id(bind.player_id);
            fb->set_reason("disconnect");
            std::string payload;
            if (flush_req.SerializeToString(&payload)) {
                class DropSink : public ReplySink {
                public:
                    void SendFrame(const std::string &) override {}
                };
                SessionHandle h;
                h.player_id = bind.player_id;
                h.connection_id = c->id();
                h.generation = bind.generation;
                h.session_id = bind.session_id;
                h.fence_token = bind.token;
#ifdef WEBSERVER_ENABLE_BRPC
                if (has_reg) {
                    h.gamelogic_instance_id = reg.gamelogic_instance_id;
                    h.map_instance_id = reg.map_instance_id;
                    h.owner_epoch = reg.map_owner_epoch;
                    h.route_version = reg.route_version;
                    if (h.session_id.empty())
                        h.session_id = reg.session_id;
                    if (h.fence_token.empty())
                        h.fence_token = reg.token;
                }
#endif
                // 无 sticky 时跳过 flush，避免 BrpcTransport missing sticky 刷屏。
                if (!h.gamelogic_instance_id.empty()) {
                    GameRequestTransport::Get().PostPlayerRequest(
                        h, std::move(payload), std::make_shared<DropSink>());
                }
            }
        }
        LOG_INFO << "Gateway disconnect conn#" << c->id() << " player_id=" << bind.player_id
                 << " generation=" << bind.generation;
    });
    loop.RunEvery(1.0, []() { RefreshLiveAges(); });
    LOG_INFO << "GameTcpGateway ready on " << ip_ << ":" << port_
             << " id=" << g_gateway_id << " (session bind + grace disconnect)"
             << " egress_bytes_per_sec=" << AoiMoveCoalesceLedger::EgressBudgetBytesPerSec()
             << " sendbuf_v2=" << (SendBufV2Enabled() ? 1 : 0)
             << " read_backpressure=" << (ReadBackpressureEnabled() ? 1 : 0);
    g_tcp_listening.store(true, std::memory_order_release);
    HealthProbeStore::SetGatewayTcpListening(true);
    server->Start();
    g_tcp_listening.store(false, std::memory_order_release);
    HealthProbeStore::SetGatewayTcpListening(false);
    server_.store(nullptr);
    loop_.store(nullptr);
#ifdef WEBSERVER_ENABLE_REDIS
    GatewayDisconnectAsync::Instance().Stop(std::chrono::milliseconds(2000));
#endif
    RpcOffloadPool::Instance().Stop();
}

void GameTcpGateway::OnMessage(const std::shared_ptr<TcpConnection> &conn) {
    const auto msg_t0 = std::chrono::steady_clock::now();
    struct MsgWatch {
        std::chrono::steady_clock::time_point t0;
        size_t frames = 0;
        ~MsgWatch() {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            ServerStats::gateway_onmessage_us.fetch_add(static_cast<uint64_t>(us > 0 ? us : 0),
                                                        std::memory_order_relaxed);
            ServerStats::gateway_onmessage_n.fetch_add(1, std::memory_order_relaxed);
            ServerStats::gateway_batch_frames.fetch_add(frames, std::memory_order_relaxed);
            OnMessageUsSamples().Add(us > 0 ? us : 0);
            BatchFramesSamples().Add(static_cast<int64_t>(frames));
        }
    } msg_watch{msg_t0, 0};
    Buffer *rb = conn->read_buf();
    if (!rb || rb->readablebytes() <= 0)
        return;
    if (ReadBackpressureEnabled()) {
        const size_t depth = ControlResponseOutbox::Instance().Depth(conn->id());
        const size_t bytes = ControlResponseOutbox::Instance().QueuedBytes(conn->id());
        const bool high = ReplyQueueHigh(depth, bytes);
        const bool low = ReplyQueueLow(depth, bytes);
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        if (conn->read_paused() && (low || conn->ReadPauseAgeMs(now) >= ReadPauseMaxMs())) {
            const int64_t credit = conn->EndReadPause(now);
            GatewayConnGuard::Instance().AddIdleCredit(conn->id(), credit);
        } else if (!conn->read_paused() && high) {
            conn->SetReadEnabled(false);
        }
    }

    const size_t nread = static_cast<size_t>(rb->readablebytes());
    OpsMetrics::Instance().AddGatewayRxBytes(nread);

    std::vector<InboundFrame> frames;
    const std::string raw = rb->PeekAllAsString();
    size_t offset = 0;
    size_t parses = 0;
    constexpr size_t kScanCap = 256;
    if (!DecodeInboundFrames(raw, kScanCap, &frames, &offset, &parses)) {
        OpsMetrics::Instance().IncIllegalFrame();
        LOG_WARN << "GameTcpGateway invalid frame conn#" << conn->id()
                 << " stream_bytes=" << raw.size();
        rb->RetrieveAll();
        conn->HandleClose();
        return;
    }
    (void)parses;
    for (size_t i = 0; i < frames.size(); ++i)
        OpsMetrics::Instance().AddGatewayRxFrames(1);
    if (offset > 0)
        rb->Retrieve(static_cast<int>(offset));
    if (frames.size() > 1) {
        std::stable_sort(frames.begin(), frames.end(),
                         [](const InboundFrame &a, const InboundFrame &b) { return a.rank < b.rank; });
    }
    if (static_cast<size_t>(rb->readablebytes()) > conn->max_read_buf_bytes()) {
        LOG_WARN << "GameTcpGateway stream overflow conn#" << conn->id();
        rb->RetrieveAll();
        conn->HandleClose();
        return;
    }
    const size_t kMaxFramesPerWake = static_cast<size_t>(FramesPerWake());
    msg_watch.frames = frames.size();
    if (frames.size() > kMaxFramesPerWake) {
        std::string putback;
        for (size_t i = kMaxFramesPerWake; i < frames.size(); ++i) {
            std::string encoded;
            if (gameproto::EncodeFrame(frames[i].payload, &encoded))
                putback.append(encoded);
        }
        if (!putback.empty()) {
            const std::string tail = rb->RetrieveAllAsString();
            rb->Append(putback);
            if (!tail.empty())
                rb->Append(tail);
        }
        frames.resize(kMaxFramesPerWake);
    }

    for (auto &item : frames) {
        const std::string &frame = item.payload;
        auto tcp_sink = std::make_shared<TcpReplySink>(conn);
        GatewayConnRegistry::Bind sticky;
        const bool bound =
            GatewayConnRegistry::Instance().FindByConnection(conn->id(), &sticky);

        const std::string frame_lim =
            GatewayConnGuard::Instance().CheckFrameRate(conn->id(), frame.size());
        if (!frame_lim.empty()) {
            ServerStats::gateway_rate_drop_frames.fetch_add(1, std::memory_order_relaxed);
            if (GatewayConnGuard::Instance().AllowRateError(conn->id()))
                SendPublicErr(tcp_sink, conn->id(), 0, gameproto::kErrRateLimited, "frame rate limited");
            if (GatewayConnGuard::Instance().RateLimitedTooLong(conn->id(), 10000)) {
                ServerStats::gateway_rate_close.fetch_add(1, std::memory_order_relaxed);
                conn->CloseForSlowClient("frame_rate");
                return;
            }
            continue;
        }
        const size_t cap = static_cast<size_t>(bound ? FrameCapLogin() : FrameCapPreLogin());
        if (ReadBackpressureEnabled() && item.rank > 1 && RpcOffloadPool::Instance().Saturated()) {
            ServerStats::gateway_busy_replies.fetch_add(1, std::memory_order_relaxed);
            if (GatewayConnGuard::Instance().AllowRateError(conn->id()))
                SendPublicErr(tcp_sink, conn->id(), 0, gameproto::kErrOverloaded, "busy");
            continue;
        }
        const bool wide = item.ok && (item.req.has_world_snapshot() || item.req.has_enter_map());
        if (!wide && frame.size() > cap) {
            LOG_WARN << "GameTcpGateway frame too large conn#" << conn->id() << " bytes=" << frame.size();
            conn->CloseForSlowClient("frame_too_large");
            return;
        }
        GatewayConnGuard::Instance().NoteActivity(conn->id(), frame.size());

        game::GameRequest peek = item.req;
        if (!item.ok) {
            OpsMetrics::Instance().IncIllegalFrame();
            SendPublicErr(tcp_sink, conn->id(), 0, gameproto::kErrInvalidArgument, "invalid protobuf");
            LOG_WARN << "GameTcpGateway illegal protobuf conn#" << conn->id();
            conn->HandleClose();
            return;
        }

        const char *pkt_kind = "other";
        if (peek.has_client_hello())
            pkt_kind = "hello";
        else if (peek.has_heartbeat())
            pkt_kind = "heartbeat";
        else if (peek.has_reconnect())
            pkt_kind = "reconnect";
        else if (peek.has_login())
            pkt_kind = "login";
        else if (peek.has_register_())
            pkt_kind = "register";
        else if (peek.has_enter_map())
            pkt_kind = "enter_map";
        else if (peek.has_query_map_lines())
            pkt_kind = "query_lines";
        else if (peek.has_move())
            pkt_kind = "move";
        else if (peek.has_world_snapshot())
            pkt_kind = "snapshot";
        else if (peek.has_friend_list())
            pkt_kind = "friend";
        else if (peek.has_push_ack())
            pkt_kind = "push_ack";
        struct HandlerSpan {
            const char *kind = "other";
            uint64_t conn_id = 0;
            uint64_t player = 0;
            uint64_t seq = 0;
            TcpConnection *live = nullptr;
            bool sample = false;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~HandlerSpan() {
                if (!live)
                    return;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                int app = 0;
                int unsent = 0;
                if (ms >= 50 || sample) {
                    app = live->send_buf() ? live->send_buf()->readablebytes() : 0;
                    unsent = live->unsent_bytes();
                    NoteSendPending(app, unsent);
                }
                NotePktStage(kind, "handler", ms, conn_id, player, seq, app, unsent);
            }
        } handler_span;
        handler_span.kind = pkt_kind;
        handler_span.conn_id = conn->id();
        handler_span.player = bound ? sticky.player_id : 0;
        handler_span.seq = peek.seq();
        handler_span.live = conn.get();
        ReplyStage::Inc(pkt_kind, ReplyStage::RequestReceived);
        if (peek.seq() != 0)
            ReplyStage::NoteRequest(conn->id(), peek.seq(), pkt_kind);
        handler_span.sample = peek.has_heartbeat() || peek.has_reconnect() || peek.has_login() ||
                              peek.has_enter_map() || peek.has_query_map_lines() ||
                              peek.has_world_snapshot() || peek.has_friend_list();

        if (peek.has_client_hello()) {
            game::GameResponse outer;
            outer.set_seq(peek.seq());
            game::ServerHelloRsp hello;
            gameproto::HandleClientHello(peek.client_hello(), conn->id(), &hello, &outer);
            hello.set_gameplay_config_version(MapCatalog::Instance().gameplay_config_version());
            hello.set_map_manifest_version(MapCatalog::Instance().map_manifest_version());
            hello.clear_maps();
            for (const auto &ent : MapCatalog::Instance().ManifestEntries()) {
                auto *m = hello.add_maps();
                m->set_map_template_id(ent.map_template_id);
                m->set_data_version(ent.data_version);
                m->set_sha256(ent.sha256);
                m->set_scene_name(ent.scene_name);
                m->set_kind(SceneKindToString(ent.policy.kind));
                if (ent.visual_map_template_id != 0)
                    m->set_visual_map_template_id(ent.visual_map_template_id);
                for (const auto &p : ent.portals) {
                    auto *pp = m->add_portals();
                    pp->set_portal_id(p.portal_id);
                    pp->set_from_map_template_id(p.from_map_template_id);
                    pp->set_to_map_template_id(p.to_map_template_id);
                    pp->mutable_position()->set_x(p.position.x);
                    pp->mutable_position()->set_y(p.position.y);
                    pp->mutable_position()->set_z(p.position.z);
                    pp->set_yaw(p.yaw);
                    pp->set_trigger_radius(p.trigger_radius);
                }
            }
            *outer.mutable_server_hello() = hello;
            if (hello.ok()) {
                GatewayConnGuard::Instance().SetHelloOk(conn->id(), true);
                OpsMetrics::Instance().IncHelloOk();
                static std::atomic<uint32_t> hello_logs{0};
                if ((hello_logs.fetch_add(1, std::memory_order_relaxed) & 31) == 0) {
                    LOG_INFO << "ClientHello ok conn#" << conn->id()
                             << " platform=" << peek.client_hello().platform()
                             << " client_version=" << peek.client_hello().client_version()
                             << " maps=" << hello.maps_size()
                             << " manifest_ver=" << hello.map_manifest_version();
                }
            } else {
                GatewayConnGuard::Instance().SetHelloOk(conn->id(), false);
                OpsMetrics::Instance().IncHelloFail();
                LOG_WARN << "ClientHello fail conn#" << conn->id()
                         << " code=" << hello.error_code();
            }
            std::string body, out;
            if (outer.SerializeToString(&body) && gameproto::EncodeFrame(body, &out))
                tcp_sink->SendFrame(out);
            continue;
        }

        if (peek.has_heartbeat()) {
            if (ClientHelloRequired() && !GatewayConnGuard::Instance().HelloOk(conn->id())) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                              "hello required");
                continue;
            }
            const std::string hb_lim =
                GatewayConnGuard::Instance().CheckHeartbeatRate(conn->id(), bound);
            if (!hb_lim.empty()) {
                OpsMetrics::Instance().IncHeartbeatLimited();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "heartbeat rate limited");
                continue;
            }
            game::GameResponse outer;
            outer.set_seq(peek.seq());
            outer.set_ok(true);
            auto *hb = outer.mutable_heartbeat();
            hb->set_ok(true);
            hb->set_error_code(gameproto::kErrOk);
            hb->set_server_time_ms(gameproto::PublicNowMs());
            hb->set_echo_ms(peek.heartbeat().echo_ms());
            hb->set_server_recv_ms(gameproto::PublicNowMs());
            hb->set_jitter_hint_ms(gameproto::HeartbeatIntervalMs() / 5);
            gameproto::PromotePublicError(&outer, conn->id());
            std::string body, out;
            // 心跳在本连接 EventLoop 上直接写。写不进缓冲就进有序队列，不关连接也不丢应答。
            int hb_rc = -1;
            if (outer.SerializeToString(&body) && gameproto::EncodeFrame(body, &out)) {
                ReplyStage::Inc("heartbeat", ReplyStage::GatewayCallbackReceived);
                ReplyStage::Inc("heartbeat", ReplyStage::LogicResponseReady);
                ReplyStage::Inc("heartbeat", ReplyStage::EventLoopEnqueued);
                OpsMetrics::Instance().IncReplyGenerated();
                hb_rc = SubmitControlOnLoop(conn, peek.seq(), "heartbeat", out);
            } else {
                ReplyStage::Inc("heartbeat", ReplyStage::ConnectionClosedBeforeReply);
            }
            const auto hb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - handler_span.t0)
                                   .count();
            if (hb_ms >= 500) {
                OpsMetrics::Instance().IncSlowReply("heartbeat");
                LOG_WARN << "slow heartbeat conn#" << conn->id() << " ms=" << hb_ms
                         << " wrote=" << (hb_rc > 0 ? 1 : 0) << " queued=" << (hb_rc == 0 ? 1 : 0)
                         << " send_buf="
                         << (conn->send_buf() ? conn->send_buf()->readablebytes() : 0)
                         << " unsent=" << conn->unsent_bytes()
                         << " loop_todo=" << (conn->loop() ? conn->loop()->TodoDepth() : 0);
            }
            continue;
        }

        if (ClientHelloRequired() && !GatewayConnGuard::Instance().HelloOk(conn->id()) &&
            (peek.has_login() || peek.has_register_() || peek.has_reconnect())) {
            SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                          "hello required");
            continue;
        }

#ifdef WEBSERVER_ENABLE_BRPC
        // 登录前白名单；未绑定业务命令 fail-closed
        if (!bound && !gameproto::IsPreAuthWhitelistPayload(frame) &&
            !gameproto::IsGatewayOwnedAuthPayload(frame)) {
            SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrUnauthenticated,
                          "unauthenticated");
            continue;
        }

        // Login / Logout / Reconnect：投递到 PlayerSerialQueue，避免阻塞 Reactor
        if (gameproto::IsGatewayOwnedAuthPayload(frame)) {
            game::GameRequest peek;
            peek.ParseFromString(frame);
            // 摘流：拒绝新 Login/Register；Reconnect/Logout 仍放行以便迁移
            if (ServiceHealth::Instance().draining() &&
                (peek.has_login() || peek.has_register_())) {
                OpsMetrics::Instance().IncDrainReject();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "gateway draining");
                continue;
            }
            const std::string auth_lim = GatewayConnGuard::Instance().CheckAuthCommandRate(conn->id());
            if (!auth_lim.empty() && (peek.has_login() || peek.has_register_())) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "auth rate limited");
                continue;
            }
            const uint64_t shard_key = bound                          ? sticky.player_id
                                       : peek.has_login()              ? peek.login().player_id()
                                       : peek.has_reconnect()          ? peek.reconnect().player_id()
                                       : peek.has_logout()             ? peek.logout().player_id()
                                                                       : conn->id();
            const uint64_t conn_id = conn->id();
            const std::string gw = g_gateway_id;
            auto sink = tcp_sink;
            std::string payload = frame;

            // Register/Logout 不占用 Login/Reconnect 流程槽；Login/Reconnect 同时仅一个
            uint64_t flow_gen = 0;
            const bool needs_flow = peek.has_login() || peek.has_reconnect();
            bool admission_held = false;
            if (needs_flow) {
                if (!LoginAdmission::Instance().TryAcquire(peek.has_reconnect())) {
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "login admission");
                    continue;
                }
                admission_held = true;
            }
            if (needs_flow && !GatewayAuthFlow::Instance().TryBegin(conn_id, &flow_gen)) {
                if (admission_held)
                    LoginAdmission::Instance().Release();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "auth flow in progress");
                continue;
            }

            auto finish = [sink, conn_id, flow_gen, needs_flow](
                              bool ok, std::string out, gameproto::GatewayLoginRoute route,
                              bool is_login, bool is_reconnect, bool is_register) {
                if (is_login) {
                    if (ok)
                        OpsMetrics::Instance().IncLoginOk();
                    else
                        OpsMetrics::Instance().IncLoginFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                        } else {
                            gameproto::CompensateGatewaySession(route.player_id, route.session_id,
                                                                route.fence_token, route.generation);
                            LOG_WARN << "login bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                } else if (is_register) {
                    if (ok)
                        OpsMetrics::Instance().IncRegisterOk();
                    else
                        OpsMetrics::Instance().IncRegisterFail();
                } else if (is_reconnect) {
                    if (ok)
                        OpsMetrics::Instance().IncReconnectOk();
                    else
                        OpsMetrics::Instance().IncReconnectFail();
                    if (ok && route.player_id != 0) {
                        if (GatewayAuthFlow::Instance().AcceptCallback(conn_id, flow_gen)) {
                            RememberBind(conn_id, route.player_id, route.fence_token,
                                         route.session_id, route.generation, sink, &route);
                            if (!out.empty() && sink &&
                                GatewayAuthFlow::Instance().Alive(conn_id)) {
                                sink->SendFrame(out);
                                out.clear();
                                for (const auto &pl : route.pending_push_payloads) {
                                    std::string frame;
                                    if (pl.size() >= 4) {
                                        const uint32_t be =
                                            (static_cast<uint8_t>(pl[0]) << 24) |
                                            (static_cast<uint8_t>(pl[1]) << 16) |
                                            (static_cast<uint8_t>(pl[2]) << 8) |
                                            static_cast<uint8_t>(pl[3]);
                                        if (be + 4 == pl.size())
                                            frame = pl;
                                    }
                                    if (frame.empty() && !gameproto::EncodeFrame(pl, &frame))
                                        continue;
                                    GatewayConnRegistry::Instance().SendBySession(route.session_id,
                                                                                frame);
                                }
                            }
                        } else {
                            gameproto::CompensateGatewaySession(route.player_id, route.session_id,
                                                                route.fence_token, route.generation);
                            LOG_WARN << "reconnect bind skipped (stale flow/conn) player="
                                     << route.player_id << " conn=" << conn_id;
                            out.clear();
                        }
                    }
                }
                if (needs_flow) {
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
                    LoginAdmission::Instance().Release();
                }
                if (!out.empty() && sink &&
                    (!needs_flow || GatewayAuthFlow::Instance().Alive(conn_id))) {
                    sink->SendFrame(out);
                }
            };

            bool started = false;
            ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
            if (peek.has_login()) {
                started = gameproto::BeginOrchestrateGatewayLogin(
                    gw, conn_id, payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), true, false, false);
                    });
            } else if (peek.has_register_()) {
                started = gameproto::BeginOrchestrateGatewayRegister(
                    payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), false, false, true);
                    });
            } else if (peek.has_reconnect()) {
                started = gameproto::BeginOrchestrateGatewayReconnect(
                    gw, conn_id, payload, shard_key,
                    [finish](bool ok, std::string out, gameproto::GatewayLoginRoute route) {
                        finish(ok, std::move(out), std::move(route), false, true, false);
                    });
            } else {
                started = PlayerSerialQueue::Instance().TryPost(
                    shard_key, [gw, conn_id, payload, sink]() {
                        std::string out;
                        gameproto::GatewayLogoutResult lr;
                        if (HasAuthoritativeLogout(conn_id)) {
                            game::GameRequest req;
                            game::GameResponse rsp;
                            if (req.ParseFromString(payload))
                                rsp.set_seq(req.seq());
                            rsp.set_ok(true);
                            rsp.set_error_code("OK");
                            rsp.set_message("already offline");
                            auto *body = rsp.mutable_logout();
                            body->set_ok(true);
                            body->set_message("already offline");
                            std::string b;
                            if (rsp.SerializeToString(&b))
                                gameproto::EncodeFrame(b, &out);
                            lr.encoded = !out.empty();
                            lr.session_ok = true;
                            lr.logic_ok = true;
                        } else {
                            (void)gameproto::OrchestrateGatewayLogout(gw, conn_id, payload, &out,
                                                                      &lr);
                            if (lr.clear_bind)
                                MarkAuthoritativeLogout(conn_id);
                        }
                        if (!out.empty() && sink)
                            sink->SendFrame(out);
                    });
            }
            if (!started) {
                if (needs_flow) {
                    GatewayAuthFlow::Instance().End(conn_id, flow_gen);
                    LoginAdmission::Instance().Release();
                }
                OpsMetrics::Instance().IncQueueOverload();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                              "queue overloaded");
            }
            continue;
        }
#endif

        // 公网命令策略：Formal 封闭 GrantItem/MailDeliver；未登记 body 默认拒绝
        {
            std::string cerr;
            if (!gameproto::AllowClientTcpPayload(frame, bound, FormalModeEnabled(), &cerr)) {
                OpsMetrics::Instance().IncCommandForbidden();
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrCommandForbidden,
                              cerr.empty() ? "command forbidden" : cerr.c_str());
                continue;
            }
        }

        if (peek.has_chat_send()) {
            const std::string chat_lim = GatewayConnGuard::Instance().CheckChatRate(conn->id());
            if (!chat_lim.empty()) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "chat rate limited");
                continue;
            }
        }
        if (peek.has_get_player_brief() && !peek.get_player_brief().player_name().empty()) {
            const std::string nlim = GatewayConnGuard::Instance().CheckNameQueryRate(conn->id());
            if (!nlim.empty()) {
                SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrRateLimited,
                              "name query rate limited");
                continue;
            }
        }

        SessionHandle handle;
        handle.connection_id = conn->id();
        if (bound) {
            // 覆盖客户端身份：不信任自报 player_id / route
            handle.player_id = sticky.player_id;
            handle.session_id = sticky.session_id;
            handle.fence_token = sticky.token;
            handle.generation = sticky.generation;
            handle.gamelogic_instance_id = sticky.gamelogic_instance_id;
            handle.map_instance_id = sticky.map_instance_id;
            handle.owner_epoch = sticky.map_owner_epoch;
            handle.route_version = sticky.route_version;
        } else {
            // 仅 Register 等白名单可到此（尚未绑定）
            handle.player_id = gameproto::ExtractPlayerIdFromRequestPayload(frame);
        }

#ifdef WEBSERVER_ENABLE_BRPC
        if (bound) {
            game::GameRequest peek;
            if (peek.ParseFromString(frame) && peek.has_push_ack()) {
                game::GameResponse rsp;
                rsp.set_seq(peek.seq());
                auto *ack = rsp.mutable_push_ack();
                const uint64_t pid = sticky.player_id != 0 ? sticky.player_id
                                                          : peek.push_ack().player_id();
                const uint64_t aseq = peek.push_ack().ack_server_seq();
                const std::string ack_sid = peek.push_ack().session_id().empty()
                                               ? sticky.session_id
                                               : peek.push_ack().session_id();
                const std::string ack_fence = peek.push_ack().fence_token().empty()
                                                 ? sticky.token
                                                 : peek.push_ack().fence_token();
                const uint64_t ack_gen = peek.push_ack().generation() != 0
                                            ? peek.push_ack().generation()
                                            : sticky.generation;
                bool ok_ack = false;
                std::string ack_msg = "ack rejected";
                uint64_t trimmed = 0;
                // 仅当前 session/fence/generation 可裁剪；旧 Session ACK 不得影响新 Session
                if (pid == sticky.player_id && aseq > 0 && ack_sid == sticky.session_id &&
                    (ack_fence.empty() || ack_fence == sticky.token) &&
                    (ack_gen == 0 || ack_gen == sticky.generation)) {
#ifdef WEBSERVER_ENABLE_REDIS
                    if (PushReplayStore::Instance().Available()) {
                        auto ar = PushReplayStore::Instance().Ack(pid, ack_sid, aseq);
                        ok_ack = ar.ok();
                        trimmed = ar.trimmed_to_seq;
                        if (ar.status == PushReplayStore::AckStatus::Ok) {
                            OpsMetrics::Instance().IncPushAckOk();
                            ack_msg = "acked";
                        } else if (ar.status == PushReplayStore::AckStatus::Duplicate) {
                            OpsMetrics::Instance().IncPushAckDuplicate();
                            ack_msg = "acked";
                        } else if (ar.status == PushReplayStore::AckStatus::Ahead) {
                            OpsMetrics::Instance().IncPushAckAheadRejected();
                            ack_msg = ar.error_code.empty() ? "ERR_ACK_AHEAD" : ar.error_code;
                        } else if (ar.status == PushReplayStore::AckStatus::Stale) {
                            OpsMetrics::Instance().IncPushAckStaleRejected();
                            ack_msg = gameproto::kErrAoiResyncRequired;
                        } else if (ar.status == PushReplayStore::AckStatus::Gap) {
                            OpsMetrics::Instance().IncPushAckGap();
                            ack_msg = gameproto::kErrAoiResyncRequired;
                        } else {
                            ack_msg = ar.error_code.empty() ? "ack rejected" : ar.error_code;
                        }
                    } else {
                        ack_msg = "ERR_ACK_UNAVAILABLE";
                    }
#else
                    ack_msg = "ERR_ACK_UNAVAILABLE";
#endif
                } else {
                    OpsMetrics::Instance().IncPushAckStaleRejected();
                    ack_msg = "ERR_ACK_STALE";
                }
                ack->set_ok(ok_ack);
                ack->set_message(ack_msg);
                ack->set_trimmed_to_seq(ok_ack ? trimmed : 0);
                rsp.set_ok(ok_ack);
                rsp.set_message(ack->message());
                gameproto::PromotePublicError(&rsp, conn->id());
                std::string body, out;
                int ack_rc = -1;
                if (rsp.SerializeToString(&body) && gameproto::EncodeFrame(body, &out)) {
                    ReplyStage::Inc("push_ack", ReplyStage::GatewayCallbackReceived);
                    ReplyStage::Inc("push_ack", ReplyStage::LogicResponseReady);
                    ReplyStage::Inc("push_ack", ReplyStage::EventLoopEnqueued);
                    OpsMetrics::Instance().IncReplyGenerated();
                    ack_rc = SubmitControlOnLoop(conn, peek.seq(), "push_ack", out);
                } else {
                    ReplyStage::Inc("push_ack", ReplyStage::ConnectionClosedBeforeReply);
                }
                const auto ack_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - handler_span.t0)
                                        .count();
                if (ack_ms >= 500) {
                    OpsMetrics::Instance().IncSlowReply("push_ack");
                    LOG_WARN << "slow push_ack conn#" << conn->id() << " ms=" << ack_ms
                             << " wrote=" << (ack_rc > 0 ? 1 : 0)
                             << " queued=" << (ack_rc == 0 ? 1 : 0)
                             << " send_buf="
                             << (conn->send_buf() ? conn->send_buf()->readablebytes() : 0)
                             << " unsent=" << conn->unsent_bytes()
                             << " loop_todo=" << (conn->loop() ? conn->loop()->TodoDepth() : 0);
                }
                continue;
            }
            // EnterMap：在 PlayerSerialQueue 编排 Transfer，禁止 Reactor 同步等 brpc
            if (peek.has_enqueue_map()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewayEnqueueMap(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_switch_line()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewaySwitchLine(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_create_dungeon()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewayCreateDungeon(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_interact_portal()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewayInteractPortal(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_query_map_lines()) {
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewayQueryMapLines(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle) {
                            (void)ok;
                            (void)conn_id;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
            if (peek.has_enter_map()) {
                if (ServiceHealth::Instance().draining()) {
                    OpsMetrics::Instance().IncDrainReject();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "gateway draining");
                    continue;
                }
                const uint64_t conn_id = conn->id();
                auto sink = tcp_sink;
                SessionHandle h = handle;
                std::string payload = frame;
                ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
                if (!gameproto::BeginOrchestrateGatewayEnterMap(
                        h, payload,
                        [conn_id, sink](bool ok, std::string out, SessionHandle route) {
                            if (ok) {
                                GatewayConnRegistry::Instance().ApplyRoute(
                                    conn_id, route.gamelogic_instance_id, route.map_instance_id,
                                    route.owner_epoch, route.route_version);
                            }
                            (void)ok;
                            if (!out.empty() && sink)
                                sink->SendFrame(out);
                        })) {
                    OpsMetrics::Instance().IncQueueOverload();
                    SendPublicErr(tcp_sink, conn->id(), peek.seq(), gameproto::kErrOverloaded,
                                  "queue overloaded");
                }
                continue;
            }
        }
#endif

        ReplyStage::Inc(pkt_kind, ReplyStage::LogicDispatchStarted);
        GameRequestTransport::Get().PostPlayerRequest(handle, std::move(frame),
                                                     std::move(tcp_sink));
    }
}
