#include "MapRuntime.h"

#include "AoiEvidence.h"
#include "AoiFanoutV2.h"
#include "AoiLeaveReason.h"
#include "AoiMembership.h"
#include "Logging.h"
#include "MoveSendConfig.h"
#include "OpsMetrics.h"
#include "ServerStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void CountAoiLeave(AoiLeaveReason reason, bool unexpected) {
    const int idx = static_cast<int>(reason);
    if (idx >= 0 && idx < static_cast<int>(AoiLeaveReason::Count))
        ServerStats::aoi_leave_by_reason[idx].fetch_add(1, std::memory_order_relaxed);
    if (unexpected)
        ServerStats::aoi_unexpected_leave.fetch_add(1, std::memory_order_relaxed);
}

int LeaveCells(const MapStaticData *data, float x0, float z0, float x1, float z1) {
    if (!data)
        return -1;
    int ax = 0, az = 0, bx = 0, bz = 0;
    data->WorldToAoiCell(x0, z0, &ax, &az);
    data->WorldToAoiCell(x1, z1, &bx, &bz);
    const int dx = ax > bx ? ax - bx : bx - ax;
    const int dz = az > bz ? az - bz : bz - az;
    return dx > dz ? dx : dz;
}

bool Finite4(float a, float b, float c, float d) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

void LogMoveTrace(const char *step, const MapEntity &e, float from_x, float from_z, float dist,
                  float dt, int watchers, bool glide, bool route, bool clamped) {
    const float speed = e.move_speed > 0.f ? e.move_speed : 10.f;
    char nums[192];
    std::snprintf(nums, sizeof(nums),
                  "from=%.1f,%.1f to=%.1f,%.1f presented=%.1f,%.1f dist=%.1f dt=%.3f speed=%.1f",
                  from_x, from_z, e.x, e.z, e.PresentedX(), e.PresentedZ(), dist, dt, speed);
    LOG_INFO << "move-trace step=" << step << " player_id=" << e.player_id << " name=" << e.player_name
             << " state_seq=" << e.state_seq << " client_seq=" << e.last_client_seq << " " << nums
             << " glide=" << (glide ? 1 : 0) << " route=" << (route ? 1 : 0)
             << " clamp=" << (clamped ? 1 : 0) << " watchers=" << watchers;
}

}  // namespace

void MapRuntime::ResetClientSeq(uint64_t player_id) {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto mit = player_map_.find(player_id);
        if (mit == player_map_.end())
            return;
        auto iit = maps_.find(mit->second);
        if (iit == maps_.end() || !iit->second)
            return;
        st = iit->second;
    }
    auto inst = LockInst(st);
    auto eit = st->entities.find(player_id);
    if (eit != st->entities.end())
        eit->second.last_client_seq = 0;
}

namespace {
int64_t MapEntityCountForMetrics() { return MapRuntime::Instance().TotalEntityCount(); }

std::mutex g_rate_mu;
std::vector<int64_t> g_move_intervals_ms;

void NoteMoveInterval(int64_t delta_ms) {
    if (delta_ms <= 0)
        return;
    std::lock_guard<std::mutex> lk(g_rate_mu);
    if (g_move_intervals_ms.size() >= 512)
        g_move_intervals_ms.erase(g_move_intervals_ms.begin());
    g_move_intervals_ms.push_back(delta_ms);
}

void SampleAoiMotion(int64_t *moving, int64_t *stationary, std::vector<int64_t> *intervals_ms) {
    if (moving)
        *moving = 0;
    if (stationary)
        *stationary = 0;
    MapRuntime::Instance().CopyMotionCounts(moving, stationary);
    if (!intervals_ms)
        return;
    std::lock_guard<std::mutex> lk(g_rate_mu);
    *intervals_ms = g_move_intervals_ms;
}
}  // namespace

MapRuntime &MapRuntime::Instance() {
    static MapRuntime g;
    static int wired = (OpsMetrics::SetMapEntityCount(&MapEntityCountForMetrics),
                        OpsMetrics::SetAoiMotionSampler(&SampleAoiMotion), 0);
    (void)wired;
    return g;
}

void MapRuntime::CopyMotionCounts(int64_t *moving, int64_t *stationary) const {
    std::vector<std::shared_ptr<InstanceState>> maps;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &kv : maps_) {
            if (kv.second)
                maps.push_back(kv.second);
        }
    }
    const int64_t now = NowMs();
    int64_t mv = 0;
    int64_t stn = 0;
    for (const auto &st : maps) {
        auto lock = LockInst(st);
        for (const auto &kv : st->entities) {
            if (!kv.second.connected)
                continue;
            const float gx = kv.second.x - kv.second.PresentedX();
            const float gz = kv.second.z - kv.second.PresentedZ();
            const bool gliding = kv.second.aoi_live && (gx * gx + gz * gz) >= 0.16f;
            if (gliding || (kv.second.last_displace_ms > 0 && now >= kv.second.last_displace_ms &&
                            now - kv.second.last_displace_ms < 5000))
                ++mv;
            else
                ++stn;
        }
    }
    if (moving)
        *moving = mv;
    if (stationary)
        *stationary = stn;
}

namespace {

int64_t PercentileOf(std::vector<int64_t> values, double q) {
    if (values.empty())
        return -1;
    std::sort(values.begin(), values.end());
    size_t i = static_cast<size_t>(q * static_cast<double>(values.size() - 1));
    if (i >= values.size())
        i = values.size() - 1;
    return values[i];
}

}  // namespace

void MapRuntime::AdvancePresented(std::vector<AoiPushBatch> *batches) {
    if (!batches)
        return;
    int64_t pending_n = 0;
    std::vector<int64_t> leave_ages;
    std::vector<int64_t> visible_counts;
    bool scanned = false;
    std::vector<std::pair<uint64_t, std::shared_ptr<InstanceState>>> maps;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &kv : maps_) {
            if (kv.second)
                maps.emplace_back(kv.first, kv.second);
        }
    }
    constexpr float kTickSec = 0.2f;
    for (const auto &item : maps) {
        AoiPushBatch batch;
        batch.map_instance_id = item.first;
        InstanceState *st = item.second.get();
        {
            auto lock = LockInst(item.second);
            if (!st->data)
                continue;
        }
        // 一次持锁最多 N 个人或 2ms。放锁后 Move 才能把自己的回包写出去。
        // 复杂度：每一片 O(片内人数 × 该人的观察者数)，不是一整张图。
        // 在 AOI 内的人同步当前坐标。离开 AOI 的人发 LEAVE，客户端不能留着旧坐标的模型。
        auto sync_pose = [&](MapEntity &subject, int64_t now_ms) {
            if (AoiV2VisibleSet()) {
                SyncVisibleSubject(st, subject, now_ms, item.first, &batch);
                return;
            }
            if (AoiMembershipUnified()) {
                SyncSubjectUnified(st, subject, now_ms, item.first, &batch);
                return;
            }
            const int enter_r = ResolvedRadius(*st);
            const int leave_r = enter_r + std::max(0, AoiV2LeaveExtraCells());
            auto collect = [&](int radius, std::unordered_set<uint64_t> *out) {
                for (uint64_t vid : NeighborhoodIdsRadius(*st, subject.PresentedX(), subject.PresentedZ(), radius)) {
                    if (vid == subject.player_id)
                        continue;
                    auto rit = st->entities.find(vid);
                    if (rit == st->entities.end() || !rit->second.connected)
                        continue;
                    out->insert(vid);
                }
            };
            std::unordered_set<uint64_t> keep;
            std::unordered_set<uint64_t> fresh;
            collect(leave_r, &keep);
            collect(enter_r, &fresh);
            auto pend = st->leave_again.find(subject.player_id);
            if (pend != st->leave_again.end()) {
                std::vector<uint64_t> drop_pend;
                for (const auto &kv : pend->second) {
                    // 只在走进进入范围时停掉离开重发。外圈还没发 ENTER，不能把离开补发也停掉。
                    if (fresh.count(kv.first) != 0)
                        drop_pend.push_back(kv.first);
                }
                for (uint64_t vid : drop_pend)
                    DropPendingLeave(st, vid, subject.player_id, AoiLeaveDropReason::BackInWindow);
            }
            std::unordered_set<uint64_t> old;
            auto wit = st->watchers.find(subject.player_id);
            if (wit != st->watchers.end())
                old.insert(wit->second.begin(), wit->second.end());
            for (const auto &vis_kv : st->visible_targets) {
                if (vis_kv.second.count(subject.player_id) != 0)
                    old.insert(vis_kv.first);
            }
            std::unordered_set<uint64_t> still;
            int left = 0;
            // 一边走出窗口时，两边的客户端都要收到 LEAVE。只告诉观察者「主体走了」，
            // 主体自己的屏幕上会留下观察者的模型。
            auto notify_leave = [&](const MapEntity &gone, const MapEntity &seer) {
                EmitTo(st, &batch, item.first, seer, 2, gone);
                ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
                EmitTo(st, &batch, item.first, seer, 3, gone);
                ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
                CountAoiLeave(AoiLeaveReason::OutOfWindow,
                              InCellRadius(*st, seer.PresentedX(), seer.PresentedZ(),
                                           gone.PresentedX(), gone.PresentedZ(), enter_r));
                AoiNoteLeaveCells(static_cast<int>(AoiLeaveReason::OutOfWindow),
                                  LeaveCells(st->data.get(), seer.PresentedX(), seer.PresentedZ(),
                                             gone.PresentedX(), gone.PresentedZ()));
                RememberLeave(st, &batch, gone, now_ms);
            };
            auto erase_visible = [&](uint64_t viewer, uint64_t target) {
                auto vis = st->visible_targets.find(viewer);
                if (vis == st->visible_targets.end())
                    return;
                vis->second.erase(target);
                if (vis->second.empty())
                    st->visible_targets.erase(vis);
            };
            auto erase_watcher = [&](uint64_t target, uint64_t viewer) {
                auto w = st->watchers.find(target);
                if (w == st->watchers.end())
                    return;
                w->second.erase(viewer);
                if (w->second.empty())
                    st->watchers.erase(w);
            };
            for (uint64_t vid : old) {
                if (keep.count(vid) != 0) {
                    still.insert(vid);
                    continue;
                }
                auto rit = st->entities.find(vid);
                if (rit == st->entities.end() || !rit->second.connected)
                    continue;
                notify_leave(subject, rit->second);
                notify_leave(rit->second, subject);
                ++left;
                erase_visible(vid, subject.player_id);
                erase_visible(subject.player_id, vid);
                erase_watcher(vid, subject.player_id);
            }
            if (left > 0)
                LogMoveTrace("leave", subject, subject.PresentedX(), subject.PresentedZ(), 0.f, 0.f,
                             left, false, false, false);
            auto &live = st->watchers[subject.player_id];
            live.clear();
            for (uint64_t vid : keep)
                still.insert(vid);
            for (uint64_t vid : still) {
                auto rit = st->entities.find(vid);
                if (rit == st->entities.end() || !rit->second.connected)
                    continue;
                auto vis = st->visible_targets.find(vid);
                const bool known =
                    vis != st->visible_targets.end() && vis->second.count(subject.player_id) != 0;
                // 外圈只继续发给已经在看的人。离开后再回来，要走进进入范围才重新发 ENTER。
                // 原先在看、现在却接不上的，先给两边发 LEAVE，不能把链接悄悄丢掉。
                if (!known && fresh.count(vid) == 0) {
                    if (old.count(vid) != 0) {
                        notify_leave(subject, rit->second);
                        notify_leave(rit->second, subject);
                        ++left;
                        erase_visible(vid, subject.player_id);
                        erase_visible(subject.player_id, vid);
                        erase_watcher(vid, subject.player_id);
                    }
                    continue;
                }
                live.insert(vid);
                st->visible_targets[vid].insert(subject.player_id);
                if (!known) {
                    EmitTo(st, &batch, item.first, rit->second, 1, subject);
                    StampEnter(st, &batch, subject);
                    ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
                } else if (AllowMoveAoi(st, rit->second, subject, now_ms)) {
                    EmitTo(st, &batch, item.first, rit->second, 2, subject);
                    ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (live.empty())
                st->watchers.erase(subject.player_id);
        };
        std::vector<uint64_t> order;
        {
            auto snap = LockInst(item.second);
            order.reserve(st->entities.size());
            for (const auto &kv : st->entities)
                order.push_back(kv.first);
        }
        const int slice_n = AoiPresentSliceEntities();
        const int slice_us = AoiPresentSliceUs();
        size_t cursor = 0;
        while (cursor < order.size()) {
            {
            auto lock = LockInst(item.second);
            const auto slice_t0 = std::chrono::steady_clock::now();
            int processed = 0;
            for (; cursor < order.size(); ++cursor) {
                if (processed > 0) {
                    const auto used = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - slice_t0)
                                          .count();
                    if (processed >= slice_n || used >= slice_us)
                        break;
                }
                ++processed;
                auto found = st->entities.find(order[cursor]);
                if (found == st->entities.end())
                    continue;
                MapEntity &e = found->second;
            if (!e.connected || !e.aoi_live)
                continue;
            const float dx = e.x - e.aoi_x;
            const float dz = e.z - e.aoi_z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist < 0.4f) {
                if (std::fabs(e.vx) > 0.05f || std::fabs(e.vz) > 0.05f) {
                    e.vx = 0.f;
                    e.vz = 0.f;
                    e.move_start_ms = NowMs();
                    e.aoi_x = e.x;
                    e.aoi_z = e.z;
                    int arrived_watchers = 0;
                    auto arrived_wit = st->watchers.find(e.player_id);
                    if (arrived_wit != st->watchers.end())
                        arrived_watchers = static_cast<int>(arrived_wit->second.size());
                    LogMoveTrace("arrive", e, e.x, e.z, 0.f, 0.f, arrived_watchers, false, false,
                                 false);
                    sync_pose(e, NowMs());
                }
                continue;
            }
            const float speed = std::sqrt(e.vx * e.vx + e.vz * e.vz);
            const float step = std::min(dist, (speed > 0.05f ? speed : 10.f) * kTickSec);
            const float nx = e.aoi_x + dx / dist * step;
            const float nz = e.aoi_z + dz / dist * step;
            int ocx = 0, ocz = 0, ncx = 0, ncz = 0;
            st->data->WorldToAoiCell(e.aoi_x, e.aoi_z, &ocx, &ocz);
            st->data->WorldToAoiCell(nx, nz, &ncx, &ncz);
            RemoveFromCell(st, e);
            e.aoi_x = nx;
            e.aoi_z = nz;
            PlaceInCell(st, e);
            const bool same_cell = ocx == ncx && ocz == ncz;
            // 格子没变时成员关系不变。重扫整片视野会占住这张图的锁，后面的 Move 回包就排到几秒。
            if (same_cell && !AoiV2VisibleSet()) {
                std::vector<uint64_t> wids;
                auto wit = st->watchers.find(e.player_id);
                if (wit != st->watchers.end())
                    wids.assign(wit->second.begin(), wit->second.end());
                const int64_t now_ms = NowMs();
                for (uint64_t vid : wids) {
                    auto rit = st->entities.find(vid);
                    if (rit == st->entities.end() || !rit->second.connected)
                        continue;
                    if (!AllowMoveAoi(st, rit->second, e, now_ms))
                        continue;
                    EmitTo(st, &batch, item.first, rit->second, 2, e);
                    ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            sync_pose(e, NowMs());
            if (same_cell || AoiV2VisibleSet())
                continue;
            std::vector<uint64_t> viewers = NeighborhoodIds(*st, e.aoi_x, e.aoi_z);
            auto wit = st->watchers.find(e.player_id);
            if (wit != st->watchers.end())
                viewers.insert(viewers.end(), wit->second.begin(), wit->second.end());
            RefreshViewers(st, viewers, e.player_id, 0, item.first, &batch);
            }
            const bool last = cursor >= order.size();
            if (last) {
                const int64_t now_ms = NowMs();
                RetryPendingEnters(st, item.first, now_ms, &batch);
                RetryPendingLeaves(st, item.first, now_ms, &batch);
            }
            const auto used = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - slice_t0)
                                  .count();
            AoiNotePresentSlice(used, processed);
            int64_t prev = ServerStats::aoi_present_slice_us_max.load(std::memory_order_relaxed);
            while (used > prev &&
                   !ServerStats::aoi_present_slice_us_max.compare_exchange_weak(
                       prev, used, std::memory_order_relaxed)) {
            }
            if (!batch.events.empty()) {
                batches->push_back(std::move(batch));
                batch.map_instance_id = item.first;
            }
            if (last && AoiV2VisibleSet()) {
            scanned = true;
            const int64_t now = NowMs();
            for (const auto &book : st->notified) {
                int64_t vis = 0;
                for (const auto &note : book.second) {
                    if (note.second.pending == InstanceState::AoiPending::Leave) {
                        ++pending_n;
                        const int64_t age = now - note.second.since_ms;
                        leave_ages.push_back(age > 0 ? age : 0);
                    } else {
                        ++vis;
                    }
                }
                visible_counts.push_back(vis);
            }
            }
            }
            if (cursor < order.size()) {
                const int pause_us = AoiPresentSlicePauseUs();
                if (pause_us > 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(pause_us));
                else
                    std::this_thread::yield();
            }
        }
        if (order.empty()) {
            auto lock = LockInst(item.second);
            const int64_t now_ms = NowMs();
            RetryPendingEnters(st, item.first, now_ms, &batch);
            RetryPendingLeaves(st, item.first, now_ms, &batch);
            if (!batch.events.empty())
                batches->push_back(std::move(batch));
        }
    }
    if (scanned) {
        ServerStats::aoi_leave_pending.store(pending_n, std::memory_order_relaxed);
        ServerStats::aoi_leave_age_p50_ms.store(PercentileOf(leave_ages, 0.50),
                                                std::memory_order_relaxed);
        ServerStats::aoi_leave_age_p95_ms.store(PercentileOf(leave_ages, 0.95),
                                                std::memory_order_relaxed);
        ServerStats::aoi_leave_age_p99_ms.store(PercentileOf(leave_ages, 0.99),
                                                std::memory_order_relaxed);
        ServerStats::aoi_leave_age_max_ms.store(PercentileOf(leave_ages, 1.0),
                                                std::memory_order_relaxed);
        ServerStats::aoi_visible_p50.store(PercentileOf(visible_counts, 0.50),
                                           std::memory_order_relaxed);
        ServerStats::aoi_visible_p95.store(PercentileOf(visible_counts, 0.95),
                                           std::memory_order_relaxed);
        ServerStats::aoi_visible_p99.store(PercentileOf(visible_counts, 0.99),
                                           std::memory_order_relaxed);
        ServerStats::aoi_visible_max.store(PercentileOf(visible_counts, 1.0),
                                           std::memory_order_relaxed);
        for (int64_t n : visible_counts)
            AoiNoteVisiblePerViewer(static_cast<int>(n));
    }
    std::vector<std::pair<uint64_t, uint64_t>> queued_leaves;
    std::vector<std::shared_ptr<InstanceState>> held;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &kv : maps_) {
            if (kv.second)
                held.push_back(kv.second);
        }
    }
    for (const auto &st : held) {
        auto lock = LockInst(st);
        if (!st || st->unloaded)
            continue;
        for (const auto &sk : st->leave_again) {
            for (const auto &vk : sk.second)
                queued_leaves.emplace_back(vk.first, sk.first);
        }
    }
    AoiMeasureLeaveInconsistent(queued_leaves);
}

void MapRuntime::SetViewRadiusCells(int n) {
    if (n >= 0)
        view_radius_ = n;
}

void MapRuntime::SetMaxVisible(int n) {
    if (n < 1)
        n = 1;
    if (n > 200)
        n = 200;
    max_visible_ = n;
}

int MapRuntime::ResolvedRadius(const InstanceState &st) const {
    return st.view_radius >= 0 ? st.view_radius : view_radius_;
}

void MapRuntime::InstLock::Release() noexcept {
    if (!live_)
        return;
    live_ = false;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - held_at_)
                        .count();
    ServerStats::aoi_lock_hold_us_sum.fetch_add(static_cast<uint64_t>(us < 0 ? 0 : us),
                                                std::memory_order_relaxed);
    if (AoiInMoveTiming())
        AoiNoteMoveStage(AoiMoveStage::LockHold, us < 0 ? 0 : us, false, false);
    if (lk_.owns_lock())
        lk_.unlock();
}

MapRuntime::InstLock::InstLock(std::unique_lock<std::mutex> lk)
    : lk_(std::move(lk)), held_at_(std::chrono::steady_clock::now()), live_(lk_.owns_lock()) {}

MapRuntime::InstLock::InstLock(InstLock &&o) noexcept
    : lk_(std::move(o.lk_)), held_at_(o.held_at_), live_(o.live_) {
    o.live_ = false;
}

MapRuntime::InstLock &MapRuntime::InstLock::operator=(InstLock &&o) noexcept {
    if (this != &o) {
        Release();
        lk_ = std::move(o.lk_);
        held_at_ = o.held_at_;
        live_ = o.live_;
        o.live_ = false;
    }
    return *this;
}

MapRuntime::InstLock::~InstLock() {
    Release();
}

MapRuntime::InstLock MapRuntime::LockInst(const std::shared_ptr<InstanceState> &st) const {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(st->mu);
    const auto waited = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(waited - t0).count();
    ServerStats::aoi_lock_wait_us_sum.fetch_add(static_cast<uint64_t>(us < 0 ? 0 : us),
                                                std::memory_order_relaxed);
    ServerStats::aoi_lock_samples.fetch_add(1, std::memory_order_relaxed);
    if (AoiInMoveTiming())
        AoiNoteMoveStage(AoiMoveStage::LockWait, us < 0 ? 0 : us, false, false);
    return InstLock(std::move(lk));
}

bool MapRuntime::PublishPlayer(uint64_t player_id, uint64_t map_instance_id,
                               const std::shared_ptr<InstanceState> &st) {
    std::lock_guard<std::mutex> reg(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end() || it->second != st || st->unloaded)
        return false;
    player_map_[player_id] = map_instance_id;
    return true;
}

void MapRuntime::RetireIfEmpty(uint64_t map_instance_id, const std::shared_ptr<InstanceState> &st) {
    if (!st)
        return;
    std::lock_guard<std::mutex> reg(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end() || it->second != st)
        return;
    if (st->entity_count.load(std::memory_order_relaxed) != 0)
        return;
    maps_.erase(it);
}

uint64_t MapRuntime::CellKey(int cx, int cz) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
           static_cast<uint32_t>(cz);
}

namespace {
std::atomic<int64_t> g_debug_now_ms{0};
}

void MapRuntime::SetNowMsForTest(int64_t ms) { g_debug_now_ms.store(ms, std::memory_order_relaxed); }

int64_t MapRuntime::NowMs() {
    const int64_t forced = g_debug_now_ms.load(std::memory_order_relaxed);
    if (forced > 0)
        return forced;
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void MapRuntime::PlaceInCell(InstanceState *st, const MapEntity &e) {
    if (!st || !st->data)
        return;
    int cx = 0, cz = 0;
    st->data->WorldToAoiCell(e.PresentedX(), e.PresentedZ(), &cx, &cz);
    auto &cell = st->cells[CellKey(cx, cz)];
    if (cell.insert(e.player_id).second)
        ++st->cell_epoch;
}

void MapRuntime::RemoveFromCell(InstanceState *st, const MapEntity &e) {
    if (!st || !st->data)
        return;
    int cx = 0, cz = 0;
    st->data->WorldToAoiCell(e.PresentedX(), e.PresentedZ(), &cx, &cz);
    auto it = st->cells.find(CellKey(cx, cz));
    if (it == st->cells.end())
        return;
    if (it->second.erase(e.player_id) != 0)
        ++st->cell_epoch;
    if (it->second.empty())
        st->cells.erase(it);
}

std::unordered_set<uint64_t> MapRuntime::VisibleIds(const InstanceState &st,
                                                    const MapEntity &e) const {
    const auto t0 = std::chrono::steady_clock::now();
    const size_t cap = static_cast<size_t>(st.max_visible > 0 ? st.max_visible : max_visible_);
    struct Cand {
        uint64_t id = 0;
        float d2 = 0.f;
    };
    std::vector<Cand> cands;
    if (!st.data)
        return {};
    int cx = 0, cz = 0;
    st.data->WorldToAoiCell(e.PresentedX(), e.PresentedZ(), &cx, &cz);
    const int r = ResolvedRadius(st);
    for (int dx = -r; dx <= r; ++dx) {
        for (int dz = -r; dz <= r; ++dz) {
            auto it = st.cells.find(CellKey(cx + dx, cz + dz));
            if (it == st.cells.end())
                continue;
            for (uint64_t pid : it->second) {
                if (pid == e.player_id)
                    continue;
                auto eit = st.entities.find(pid);
                if (eit == st.entities.end() || !eit->second.connected)
                    continue;
                const float ddx = eit->second.PresentedX() - e.PresentedX();
                const float ddz = eit->second.PresentedZ() - e.PresentedZ();
                cands.push_back(Cand{pid, ddx * ddx + ddz * ddz});
            }
        }
    }
    ServerStats::aoi_candidate_sum.fetch_add(cands.size(), std::memory_order_relaxed);
    ServerStats::aoi_candidate_count.fetch_add(1, std::memory_order_relaxed);
    if (cands.size() > cap) {
        std::nth_element(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(cap),
                         cands.end(), [](const Cand &a, const Cand &b) {
                             if (a.d2 < b.d2)
                                 return true;
                             if (b.d2 < a.d2)
                                 return false;
                             return a.id < b.id;
                         });
        cands.resize(cap);
    }
    ServerStats::aoi_selected_sum.fetch_add(cands.size(), std::memory_order_relaxed);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ServerStats::ObserveAoiQueryUs(static_cast<uint64_t>(us < 0 ? 0 : us));
    std::unordered_set<uint64_t> out;
    out.reserve(cands.size());
    for (const auto &c : cands)
        out.insert(c.id);
    return out;
}

std::vector<uint64_t> MapRuntime::NeighborhoodIdsRadius(const InstanceState &st, float x, float z,
                                                         int radius) const {
    std::vector<uint64_t> ids;
    if (!st.data || radius < 0)
        return ids;
    int cx = 0, cz = 0;
    st.data->WorldToAoiCell(x, z, &cx, &cz);
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const uint64_t key = CellKey(cx + dx, cz + dz);
            auto cit = st.cells.find(key);
            if (cit == st.cells.end())
                continue;
            for (uint64_t pid : cit->second)
                ids.push_back(pid);
        }
    }
    return ids;
}

std::vector<uint64_t> MapRuntime::NeighborhoodIds(const InstanceState &st, float x, float z) const {
    return NeighborhoodIdsRadius(st, x, z, ResolvedRadius(st));
}

bool MapRuntime::InCellRadius(const InstanceState &st, float x0, float z0, float x1, float z1,
                              int radius) const {
    if (!st.data || radius < 0)
        return false;
    int ax = 0, az = 0, bx = 0, bz = 0;
    st.data->WorldToAoiCell(x0, z0, &ax, &az);
    st.data->WorldToAoiCell(x1, z1, &bx, &bz);
    const int dx = ax > bx ? ax - bx : bx - ax;
    const int dz = az > bz ? az - bz : bz - az;
    return dx <= radius && dz <= radius;
}

void MapRuntime::SyncVisibleSubject(InstanceState *st, MapEntity &subject, int64_t now_ms,
                                    uint64_t map_id, AoiPushBatch *batch) {
    if (!st || subject.player_id == 0)
        return;
    const int enter_r = ResolvedRadius(*st);
    int leave_r = enter_r;
    if (AoiV2Hysteresis())
        leave_r = enter_r + AoiV2LeaveExtraCells();
    const int cap = AoiMembershipUnified() ? AoiUnifiedCap()
                                           : (st->max_visible > 0 ? st->max_visible : max_visible_);
    const int slack = AoiV2CapSlack();
    std::unordered_set<uint64_t> viewers;
    for (uint64_t id : NeighborhoodIdsRadius(*st, subject.PresentedX(), subject.PresentedZ(), leave_r)) {
        if (id != subject.player_id)
            viewers.insert(id);
    }
    for (const auto &vk : st->notified) {
        if (vk.second.count(subject.player_id) != 0)
            viewers.insert(vk.first);
    }
    auto stamp = [&](uint64_t version) {
        if (batch && !batch->events.empty())
            batch->events.back().notify_version = version;
    };
    for (uint64_t viewer_id : viewers) {
        auto vit = st->entities.find(viewer_id);
        if (vit == st->entities.end() || !vit->second.connected)
            continue;
        const MapEntity &viewer = vit->second;
        auto &book = st->notified[viewer_id];
        std::vector<AoiCand> cands;
        std::unordered_set<uint64_t> seen;
        for (uint64_t id : NeighborhoodIdsRadius(*st, viewer.PresentedX(), viewer.PresentedZ(), leave_r)) {
            if (id == viewer_id)
                continue;
            auto eit = st->entities.find(id);
            if (eit == st->entities.end() || !eit->second.connected)
                continue;
            const float dx = eit->second.PresentedX() - viewer.PresentedX();
            const float dz = eit->second.PresentedZ() - viewer.PresentedZ();
            AoiCand c;
            c.id = id;
            c.d2 = dx * dx + dz * dz;
            c.in_leave = true;
            c.in_enter = InCellRadius(*st, viewer.PresentedX(), viewer.PresentedZ(),
                                      eit->second.PresentedX(), eit->second.PresentedZ(), enter_r);
            auto nit = book.find(id);
            c.in_notified = nit != book.end() && nit->second.pending != InstanceState::AoiPending::Leave;
            cands.push_back(c);
            seen.insert(id);
        }
        for (const auto &nk : book) {
            if (seen.count(nk.first) != 0)
                continue;
            AoiCand c;
            c.id = nk.first;
            c.d2 = 1e12f;
            c.in_notified = nk.second.pending != InstanceState::AoiPending::Leave;
            cands.push_back(c);
        }
        const std::vector<uint64_t> desired = SelectVisible(cands, cap, slack);
        std::unordered_set<uint64_t> want(desired.begin(), desired.end());
        std::unordered_map<uint64_t, bool> in_leave_of;
        in_leave_of.reserve(cands.size());
        for (const auto &c : cands)
            in_leave_of[c.id] = c.in_leave;
        auto in_window = [&](uint64_t id) {
            auto it = in_leave_of.find(id);
            return it != in_leave_of.end() && it->second;
        };
        auto unlink_id = [&](uint64_t id) {
            auto w = st->watchers.find(id);
            if (w != st->watchers.end()) {
                w->second.erase(viewer_id);
                if (w->second.empty())
                    st->watchers.erase(w);
            }
            auto vis = st->visible_targets.find(viewer_id);
            if (vis != st->visible_targets.end()) {
                vis->second.erase(id);
                if (vis->second.empty())
                    st->visible_targets.erase(vis);
            }
        };
        auto link_id = [&](uint64_t id) {
            st->watchers[id].insert(viewer_id);
            st->visible_targets[viewer_id].insert(id);
        };
        auto entity_of = [&](uint64_t id) {
            auto eit = st->entities.find(id);
            if (eit != st->entities.end())
                return eit->second;
            MapEntity stub;
            stub.player_id = id;
            return stub;
        };
        std::vector<uint64_t> booked;
        booked.reserve(book.size());
        for (const auto &nk : book)
            booked.push_back(nk.first);
        for (uint64_t id : booked) {
            auto it = book.find(id);
            if (it == book.end() || want.count(id) != 0)
                continue;
            if (it->second.pending == InstanceState::AoiPending::Leave) {
                if (now_ms - it->second.since_ms >= 200) {
                    EmitTo(st, batch, map_id, viewer, 3, entity_of(id));
                    ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
                    CountAoiLeave(AoiLeaveReason::RetryResend, false);
                    stamp(it->second.version);
                    it->second.since_ms = now_ms;
                }
                continue;
            }
            if (in_window(id))
                ServerStats::aoi_cap_evict.fetch_add(1, std::memory_order_relaxed);
            {
                bool inside = false;
                auto eit = st->entities.find(id);
                if (eit != st->entities.end() && eit->second.connected)
                    inside = InCellRadius(*st, viewer.PresentedX(), viewer.PresentedZ(),
                                          eit->second.PresentedX(), eit->second.PresentedZ(),
                                          ResolvedRadius(*st));
                const auto why = in_window(id) ? AoiLeaveReason::CapEvict : AoiLeaveReason::OutOfWindow;
                CountAoiLeave(why, inside);
                if (eit != st->entities.end())
                    AoiNoteLeaveCells(static_cast<int>(why),
                                      LeaveCells(st->data.get(), viewer.PresentedX(), viewer.PresentedZ(),
                                                 eit->second.PresentedX(), eit->second.PresentedZ()));
            }
            unlink_id(id);
            it->second.pending = InstanceState::AoiPending::Leave;
            it->second.version += 1;
            it->second.since_ms = now_ms;
            EmitTo(st, batch, map_id, viewer, 3, entity_of(id));
            ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
            stamp(it->second.version);
            auto entered = st->enter_again.find(id);
            if (entered != st->enter_again.end()) {
                entered->second.erase(viewer_id);
                if (entered->second.empty())
                    st->enter_again.erase(entered);
            }
        }
        for (uint64_t id : desired) {
            auto it = book.find(id);
            const bool has = it != book.end();
            const bool stable = has && it->second.pending != InstanceState::AoiPending::Leave;
            if (!stable) {
                link_id(id);
                auto &item = book[id];
                item.pending = InstanceState::AoiPending::Enter;
                item.version += 1;
                item.since_ms = now_ms;
                auto left = st->last_left_ms[viewer_id].find(id);
                if (left != st->last_left_ms[viewer_id].end() && now_ms - left->second < 10000 &&
                    now_ms >= left->second)
                    ServerStats::aoi_edge_flap.fetch_add(1, std::memory_order_relaxed);
                EmitTo(st, batch, map_id, viewer, 1, entity_of(id));
                ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
                stamp(item.version);
                StampEnter(st, batch, entity_of(id));
            } else if (id == subject.player_id) {
                link_id(id);
                if (AllowMoveAoi(st, viewer, subject, now_ms))
                    EmitTo(st, batch, map_id, viewer, 2, subject);
            } else {
                link_id(id);
            }
        }
    }
}

std::unordered_set<uint64_t> MapRuntime::DesiredVisible(const InstanceState &st,
                                                        const MapEntity &viewer) const {
    const int enter_r = ResolvedRadius(st);
    const int leave_r = enter_r + std::max(0, AoiV2LeaveExtraCells());
    const int cap = AoiUnifiedCap();
    int slack = AoiV2CapSlack();
    if (slack >= cap)
        slack = cap - 1;
    std::vector<AoiCand> cands;
    auto vis = st.visible_targets.find(viewer.player_id);
    for (uint64_t id : NeighborhoodIdsRadius(st, viewer.PresentedX(), viewer.PresentedZ(), leave_r)) {
        if (id == viewer.player_id)
            continue;
        auto eit = st.entities.find(id);
        if (eit == st.entities.end() || !eit->second.connected)
            continue;
        AoiCand c;
        c.id = id;
        const float dx = eit->second.PresentedX() - viewer.PresentedX();
        const float dz = eit->second.PresentedZ() - viewer.PresentedZ();
        c.d2 = dx * dx + dz * dz;
        c.in_leave = true;
        c.in_enter = InCellRadius(st, viewer.PresentedX(), viewer.PresentedZ(), eit->second.PresentedX(),
                                  eit->second.PresentedZ(), enter_r);
        c.in_notified = vis != st.visible_targets.end() && vis->second.count(id) != 0;
        cands.push_back(c);
    }
    const auto ids = SelectVisible(cands, cap, slack);
    return std::unordered_set<uint64_t>(ids.begin(), ids.end());
}

void MapRuntime::SyncSubjectUnified(InstanceState *st, MapEntity &subject, int64_t now_ms,
                                    uint64_t map_id, AoiPushBatch *batch) {
    if (!st)
        return;
    const int enter_r = ResolvedRadius(*st);
    const int leave_r = enter_r + std::max(0, AoiV2LeaveExtraCells());
    std::unordered_set<uint64_t> viewers;
    for (uint64_t id : NeighborhoodIdsRadius(*st, subject.PresentedX(), subject.PresentedZ(), leave_r)) {
        if (id != subject.player_id)
            viewers.insert(id);
    }
    auto wit = st->watchers.find(subject.player_id);
    if (wit != st->watchers.end())
        viewers.insert(wit->second.begin(), wit->second.end());
    for (uint64_t vid : viewers) {
        auto rit = st->entities.find(vid);
        if (rit == st->entities.end() || !rit->second.connected)
            continue;
        const auto desired = DesiredVisible(*st, rit->second);
        const bool want = desired.count(subject.player_id) != 0;
        auto vis = st->visible_targets.find(vid);
        const bool known = vis != st->visible_targets.end() && vis->second.count(subject.player_id) != 0;
        const bool in_enter = InCellRadius(*st, rit->second.PresentedX(), rit->second.PresentedZ(),
                                           subject.PresentedX(), subject.PresentedZ(), enter_r);
        // 外圈不把没看过的人记成已可见，否则走进进入范围时不会再发 ENTER。
        if (want && (known || in_enter)) {
            st->watchers[subject.player_id].insert(vid);
            st->visible_targets[vid].insert(subject.player_id);
            if (!known) {
                EmitTo(st, batch, map_id, rit->second, 1, subject);
                StampEnter(st, batch, subject);
                ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
            } else if (AllowMoveAoi(st, rit->second, subject, now_ms)) {
                EmitTo(st, batch, map_id, rit->second, 2, subject);
                ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (!known)
            continue;
        const bool inside = InCellRadius(*st, rit->second.PresentedX(), rit->second.PresentedZ(),
                                         subject.PresentedX(), subject.PresentedZ(), enter_r);
        EmitTo(st, batch, map_id, rit->second, 2, subject);
        EmitTo(st, batch, map_id, rit->second, 3, subject);
        ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
        CountAoiLeave(inside ? AoiLeaveReason::CapEvict : AoiLeaveReason::OutOfWindow, inside);
        RememberLeave(st, batch, subject, now_ms);
        st->visible_targets[vid].erase(subject.player_id);
        if (st->visible_targets[vid].empty())
            st->visible_targets.erase(vid);
        auto live = st->watchers.find(subject.player_id);
        if (live != st->watchers.end()) {
            live->second.erase(vid);
            if (live->second.empty())
                st->watchers.erase(live);
        }
    }
}

void MapRuntime::RememberLeave(InstanceState *st, AoiPushBatch *batch, const MapEntity &subject,
                               int64_t now_ms) {
    if (!st || !batch || batch->events.empty() || subject.player_id == 0)
        return;
    auto &ev = batch->events.back();
    if (ev.op != 3 || ev.snapshot.player_id != subject.player_id || ev.recipient_id == 0)
        return;
    auto entered = st->enter_again.find(subject.player_id);
    if (entered != st->enter_again.end()) {
        entered->second.erase(ev.recipient_id);
        if (entered->second.empty())
            st->enter_again.erase(entered);
    }
    auto &again = st->leave_again[subject.player_id][ev.recipient_id];
    if (again.version == 0) {
        if (st->next_leave_version == 0)
            st->next_leave_version = 1;
        again.version = st->next_leave_version++;
        again.tries = 1;
        again.first_ms = now_ms;
    } else {
        again.tries += 1;
    }
    again.last_ms = now_ms;
    again.snap = subject;
    ev.notify_version = again.version;
    st->interest_version[subject.player_id][ev.recipient_id] = again.version;
}

void MapRuntime::StampEnter(InstanceState *st, AoiPushBatch *batch, const MapEntity &subject) {
    if (!st || !batch || batch->events.empty() || subject.player_id == 0)
        return;
    auto &ev = batch->events.back();
    if (ev.op != 1 || ev.snapshot.player_id != subject.player_id || ev.recipient_id == 0)
        return;
    uint64_t ver = ev.notify_version;
    if (ver == 0) {
        auto pending = st->enter_again.find(subject.player_id);
        if (pending != st->enter_again.end()) {
            auto it = pending->second.find(ev.recipient_id);
            if (it != pending->second.end() && it->second.version != 0)
                ver = it->second.version;
        }
    }
    if (ver == 0) {
        if (st->next_leave_version == 0)
            st->next_leave_version = 1;
        ver = st->next_leave_version++;
    }
    ev.notify_version = ver;
    auto &cur = st->interest_version[subject.player_id][ev.recipient_id];
    if (ver > cur)
        cur = ver;
}

namespace {

int64_t LeaveRetryGapMs(int tries_sent) {
    if (tries_sent <= 1)
        return 1000;
    if (tries_sent == 2)
        return 2000;
    if (tries_sent == 3)
        return 4000;
    return 5000;
}

int64_t ViewerLeaveBackoffMs(int streak) {
    int64_t gap = 1000;
    const int n = streak < 1 ? 1 : streak;
    for (int i = 1; i < n; ++i) {
        if (gap >= 30000)
            return 30000;
        gap *= 2;
    }
    if (gap > 30000)
        gap = 30000;
    return gap;
}

int64_t EnterRetryGapMs(int tries_sent) {
    if (tries_sent <= 1)
        return 200;
    if (tries_sent == 2)
        return 1000;
    if (tries_sent == 3)
        return 2000;
    return 5000;
}

}  // namespace

void MapRuntime::DropPendingLeave(InstanceState *st, uint64_t viewer, uint64_t subject,
                                  AoiLeaveDropReason reason) {
    if (!st || viewer == 0 || subject == 0)
        return;
    bool removed_queue = false;
    auto sit = st->leave_again.find(subject);
    if (sit != st->leave_again.end()) {
        removed_queue = sit->second.erase(viewer) > 0;
        if (sit->second.empty())
            st->leave_again.erase(sit);
    }
    const bool removed_evidence =
        AoiTakeLeaveFail(viewer, subject, NowMs(), reason == AoiLeaveDropReason::Acked);
    if (removed_queue || removed_evidence)
        AoiCountLeaveDrop(reason);
}

void MapRuntime::RetryPendingLeaves(InstanceState *st, uint64_t map_id, int64_t now_ms,
                                    AoiPushBatch *batch) {
    if (!st || !batch)
        return;
    struct Due {
        int64_t last_ms = 0;
        uint64_t sid = 0;
        uint64_t vid = 0;
        bool correct = false;
    };
    std::vector<Due> due;
    struct Drop {
        uint64_t sid = 0;
        uint64_t vid = 0;
        AoiLeaveDropReason reason = AoiLeaveDropReason::GivenUp;
    };
    std::vector<Drop> drop;
    const int enter_r = ResolvedRadius(*st);
    const int leave_r = enter_r + std::max(0, AoiV2LeaveExtraCells());
    const int64_t max_age = AoiLeaveRetryMaxMs();
    for (const auto &sk : st->leave_again) {
        const uint64_t sid = sk.first;
        auto eit = st->entities.find(sid);
        const bool live = eit != st->entities.end() && eit->second.connected;
        for (const auto &vk : sk.second) {
            const uint64_t vid = vk.first;
            const auto &again = vk.second;
            auto vit = st->entities.find(vid);
            if (vit == st->entities.end() || !vit->second.connected) {
                drop.push_back(Drop{sid, vid, AoiLeaveDropReason::ViewerOffline});
                continue;
            }
            auto known = st->interest_version[sid].find(vid);
            if (known != st->interest_version[sid].end() && known->second > again.version) {
                drop.push_back(Drop{sid, vid, AoiLeaveDropReason::VersionSuperseded});
                continue;
            }
            int64_t first = AoiLeaveFailSince(vid, sid);
            if (first <= 0)
                first = again.first_ms > 0 ? again.first_ms : again.last_ms;
            if (first > 0 && now_ms >= first && now_ms - first >= max_age) {
                static int64_t leave_giveup_log_ms = 0;
                if (leave_giveup_log_ms == 0 || now_ms < leave_giveup_log_ms ||
                    now_ms - leave_giveup_log_ms >= 1000) {
                    leave_giveup_log_ms = now_ms;
                    LOG_WARN << "AOI leave retry expired viewer=" << vid << " subject=" << sid
                             << " age_ms=" << (now_ms - first);
                }
                ServerStats::aoi_leave_retry_expired.fetch_add(1, std::memory_order_relaxed);
                drop.push_back(Drop{sid, vid, AoiLeaveDropReason::GivenUp});
                continue;
            }
            const int unsent = AoiViewerUnsentBytes(vid);
            if (unsent > 16384) {
                auto &b = st->leave_backoff[vid];
                if (b.next_ms <= now_ms) {
                    if (b.streak < 16)
                        ++b.streak;
                    if (b.streak < 1)
                        b.streak = 1;
                    b.next_ms = now_ms + ViewerLeaveBackoffMs(b.streak);
                }
                continue;
            }
            auto bit = st->leave_backoff.find(vid);
            if (bit != st->leave_backoff.end() && bit->second.next_ms > now_ms)
                continue;
            if (live) {
                bool in_window = false;
                for (uint64_t id : NeighborhoodIdsRadius(*st, eit->second.PresentedX(),
                                                         eit->second.PresentedZ(), leave_r)) {
                    if (id == vid) {
                        in_window = true;
                        break;
                    }
                }
                if (in_window) {
                    Due d;
                    d.last_ms = again.last_ms;
                    d.sid = sid;
                    d.vid = vid;
                    d.correct = true;
                    due.push_back(d);
                    continue;
                }
            }
            if (now_ms - again.last_ms < LeaveRetryGapMs(again.tries))
                continue;
            Due d;
            d.last_ms = again.last_ms;
            d.sid = sid;
            d.vid = vid;
            due.push_back(d);
        }
    }
    std::sort(due.begin(), due.end(), [](const Due &a, const Due &b) {
        if (a.last_ms != b.last_ms)
            return a.last_ms < b.last_ms;
        if (a.sid != b.sid)
            return a.sid < b.sid;
        return a.vid < b.vid;
    });
    int sent = 0;
    for (const auto &d : due) {
        if (sent >= 32)
            break;
        auto sit = st->leave_again.find(d.sid);
        if (sit == st->leave_again.end() || sit->second.count(d.vid) == 0)
            continue;
        auto vit = st->entities.find(d.vid);
        auto eit = st->entities.find(d.sid);
        if (vit == st->entities.end() || !vit->second.connected)
            continue;
        const bool live = eit != st->entities.end() && eit->second.connected;
        MapEntity snap = live ? eit->second : sit->second[d.vid].snap;
        if (snap.player_id == 0)
            snap.player_id = d.sid;
        if (d.correct) {
            EmitTo(st, batch, map_id, vit->second, 1, snap);
            StampEnter(st, batch, snap);
            st->watchers[d.sid].insert(d.vid);
            st->visible_targets[d.vid].insert(d.sid);
            ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
            drop.push_back(Drop{d.sid, d.vid, AoiLeaveDropReason::BackInWindow});
        } else {
            EmitTo(st, batch, map_id, vit->second, 3, snap);
            ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
            CountAoiLeave(AoiLeaveReason::RetryResend, false);
            RememberLeave(st, batch, snap, now_ms);
        }
        ++sent;
    }
    for (const auto &item : drop)
        DropPendingLeave(st, item.vid, item.sid, item.reason);
}

void MapRuntime::RetryPendingEnters(InstanceState *st, uint64_t map_id, int64_t now_ms,
                                    AoiPushBatch *batch) {
    if (!st || !batch)
        return;
    struct Due {
        int64_t last_ms = 0;
        uint64_t sid = 0;
        uint64_t vid = 0;
    };
    std::vector<Due> due;
    std::vector<std::pair<uint64_t, uint64_t>> drop;
    const int leave_r = ResolvedRadius(*st) + std::max(0, AoiV2LeaveExtraCells());
    const int64_t max_age = AoiLeaveRetryMaxMs();
    for (const auto &sk : st->enter_again) {
        const uint64_t sid = sk.first;
        auto eit = st->entities.find(sid);
        const bool live = eit != st->entities.end() && eit->second.connected;
        for (const auto &vk : sk.second) {
            const uint64_t vid = vk.first;
            const auto &again = vk.second;
            auto vit = st->entities.find(vid);
            if (!live || vit == st->entities.end() || !vit->second.connected) {
                drop.emplace_back(sid, vid);
                continue;
            }
            auto versions = st->interest_version.find(sid);
            if (versions != st->interest_version.end()) {
                auto known = versions->second.find(vid);
                if (known != versions->second.end() && known->second > again.version) {
                    drop.emplace_back(sid, vid);
                    continue;
                }
            }
            auto vis = st->visible_targets.find(vid);
            if (vis == st->visible_targets.end() || vis->second.count(sid) == 0) {
                drop.emplace_back(sid, vid);
                continue;
            }
            bool in_window = false;
            for (uint64_t id :
                 NeighborhoodIdsRadius(*st, eit->second.PresentedX(), eit->second.PresentedZ(), leave_r)) {
                if (id == vid) {
                    in_window = true;
                    break;
                }
            }
            if (!in_window) {
                drop.emplace_back(sid, vid);
                continue;
            }
            const int64_t first = again.first_ms > 0 ? again.first_ms : again.last_ms;
            if (first > 0 && now_ms - first >= max_age) {
                LOG_WARN << "AOI enter retry expired viewer=" << vid << " subject=" << sid
                         << " age_ms=" << (now_ms - first);
                drop.emplace_back(sid, vid);
                continue;
            }
            if (again.last_ms > 0 && now_ms - again.last_ms < EnterRetryGapMs(again.tries))
                continue;
            Due d;
            d.last_ms = again.last_ms;
            d.sid = sid;
            d.vid = vid;
            due.push_back(d);
        }
    }
    std::sort(due.begin(), due.end(), [](const Due &a, const Due &b) {
        if (a.last_ms != b.last_ms)
            return a.last_ms < b.last_ms;
        if (a.sid != b.sid)
            return a.sid < b.sid;
        return a.vid < b.vid;
    });
    int sent = 0;
    for (const auto &d : due) {
        if (sent >= 32)
            break;
        auto sit = st->enter_again.find(d.sid);
        if (sit == st->enter_again.end() || sit->second.count(d.vid) == 0)
            continue;
        auto vit = st->entities.find(d.vid);
        auto eit = st->entities.find(d.sid);
        if (vit == st->entities.end() || eit == st->entities.end() || !vit->second.connected ||
            !eit->second.connected)
            continue;
        const size_t before = batch->events.size();
        EmitTo(st, batch, map_id, vit->second, 1, eit->second);
        if (batch->events.size() == before)
            continue;
        auto &again = sit->second[d.vid];
        again.tries += 1;
        again.last_ms = now_ms;
        StampEnter(st, batch, eit->second);
        ++sent;
    }
    for (const auto &pair : drop) {
        auto cur = st->enter_again.find(pair.first);
        if (cur == st->enter_again.end())
            continue;
        cur->second.erase(pair.second);
        if (cur->second.empty())
            st->enter_again.erase(cur);
    }
}

void MapRuntime::NoteAoiWrite(uint64_t viewer_id, uint64_t subject_id, int op, bool ok,
                              uint64_t version) {
    if (viewer_id == 0 || subject_id == 0 || version == 0)
        return;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(viewer_id);
        if (pit == player_map_.end())
            return;
        auto mit = maps_.find(pit->second);
        if (mit == maps_.end())
            return;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (!st || st->unloaded)
        return;
    if (op == 3) {
        if (ok) {
            st->leave_backoff.erase(viewer_id);
            bool matched = false;
            auto pend = st->leave_again.find(subject_id);
            if (pend != st->leave_again.end()) {
                auto it = pend->second.find(viewer_id);
                matched = it != pend->second.end() && it->second.version == version;
            }
            if (matched) {
                DropPendingLeave(st.get(), viewer_id, subject_id, AoiLeaveDropReason::Acked);
            } else if (AoiTakeLeaveFail(viewer_id, subject_id, NowMs(), true)) {
                AoiCountLeaveDrop(AoiLeaveDropReason::Acked);
            }
        } else {
            AoiNoteLeaveWriteFail(viewer_id, subject_id, NowMs());
            auto &b = st->leave_backoff[viewer_id];
            if (b.streak < 16)
                ++b.streak;
            b.next_ms = NowMs() + ViewerLeaveBackoffMs(b.streak);
        }
    }
    if (op == 1) {
        auto pend = st->enter_again.find(subject_id);
        if (ok) {
            if (pend != st->enter_again.end()) {
                auto it = pend->second.find(viewer_id);
                if (it != pend->second.end() && it->second.version == version) {
                    pend->second.erase(it);
                    if (pend->second.empty())
                        st->enter_again.erase(pend);
                }
            }
            if (AoiTakeLeaveFail(viewer_id, subject_id, NowMs(), false))
                AoiCountLeaveDrop(AoiLeaveDropReason::BackInWindow);
        } else {
            const bool older = st->enter_again[subject_id].count(viewer_id) != 0 &&
                               st->enter_again[subject_id][viewer_id].version > version;
            if (!older) {
                auto &again = st->enter_again[subject_id][viewer_id];
                if (again.first_ms == 0)
                    again.first_ms = NowMs();
                if (again.tries == 0)
                    again.tries = 1;
                again.version = version;
                again.last_ms = NowMs();
                auto eit = st->entities.find(subject_id);
                if (eit != st->entities.end())
                    again.snap = eit->second;
            }
        }
    }
    if (!AoiV2VisibleSet())
        return;
    auto book = st->notified.find(viewer_id);
    if (book == st->notified.end())
        return;
    auto it = book->second.find(subject_id);
    if (it == book->second.end() || it->second.version != version)
        return;
    if (!ok)
        return;
    if (op == 3) {
        st->last_left_ms[viewer_id][subject_id] = NowMs();
        book->second.erase(it);
        if (book->second.empty())
            st->notified.erase(book);
    } else if (op == 1 && it->second.pending == InstanceState::AoiPending::Enter) {
        it->second.pending = InstanceState::AoiPending::None;
    }
}

int MapRuntime::LeavePendingCountForTest(uint64_t viewer_id) const {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(viewer_id);
        if (pit == player_map_.end())
            return 0;
        auto mit = maps_.find(pit->second);
        if (mit == maps_.end() || !mit->second)
            return 0;
        st = mit->second;
    }
    auto inst = LockInst(st);
    auto book = st->notified.find(viewer_id);
    if (book == st->notified.end())
        return 0;
    int n = 0;
    for (const auto &kv : book->second) {
        if (kv.second.pending == InstanceState::AoiPending::Leave)
            ++n;
    }
    return n;
}

void MapRuntime::ForgetViewer(InstanceState *st, uint64_t viewer_id) {
    if (!st)
        return;
    auto it = st->visible_targets.find(viewer_id);
    if (it != st->visible_targets.end()) {
        for (uint64_t tid : it->second) {
            auto w = st->watchers.find(tid);
            if (w == st->watchers.end())
                continue;
            w->second.erase(viewer_id);
            if (w->second.empty())
                st->watchers.erase(w);
        }
        st->visible_targets.erase(it);
    }
    st->move_notify_ms.erase(viewer_id);
    for (auto &kv : st->move_notify_ms)
        kv.second.erase(viewer_id);
    st->notified.erase(viewer_id);
    st->last_left_ms.erase(viewer_id);
}

void MapRuntime::RefreshViewers(InstanceState *st, const std::vector<uint64_t> &viewer_ids,
                                uint64_t mover_id, uint64_t silent_viewer, uint64_t map_instance_id,
                                AoiPushBatch *pushes) {
    if (!st)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_set<uint64_t> uniq(viewer_ids.begin(), viewer_ids.end());
    for (uint64_t viewer_id : uniq) {
        auto eit = st->entities.find(viewer_id);
        if (eit == st->entities.end() || !eit->second.connected)
            continue;
        const MapEntity &viewer = eit->second;
        std::unordered_set<uint64_t> old;
        auto oit = st->visible_targets.find(viewer_id);
        if (oit != st->visible_targets.end())
            old = oit->second;
        auto neu = AoiMembershipUnified() ? DesiredVisible(*st, viewer) : VisibleIds(*st, viewer);
        const bool emit = viewer_id != silent_viewer;
        const int radius = ResolvedRadius(*st);
        for (uint64_t tid : old) {
            if (neu.count(tid))
                continue;
            auto tit = st->entities.find(tid);
            // 还在格子视野里、只是人数封顶没选中时，不要先 LEAVE。走出格子视野必须 LEAVE。
            if (tit != st->entities.end() && tit->second.connected && st->data && radius >= 0) {
                int vx = 0, vz = 0, tx = 0, tz = 0;
                st->data->WorldToAoiCell(viewer.PresentedX(), viewer.PresentedZ(), &vx, &vz);
                st->data->WorldToAoiCell(tit->second.PresentedX(), tit->second.PresentedZ(), &tx,
                                         &tz);
                const int cdx = vx > tx ? vx - tx : tx - vx;
                const int cdz = vz > tz ? vz - tz : tz - vz;
                int keep_radius = radius + std::max(0, AoiV2LeaveExtraCells());
                if (cdx <= keep_radius && cdz <= keep_radius && !AoiV2VisibleSet() &&
                    !AoiMembershipUnified()) {
                    neu.insert(tid);
                    continue;
                }
            }
            auto w = st->watchers.find(tid);
            if (w != st->watchers.end()) {
                w->second.erase(viewer_id);
                if (w->second.empty())
                    st->watchers.erase(w);
            }
            if (!emit)
                continue;
            MapEntity snap;
            if (tit != st->entities.end())
                snap = tit->second;
            else
                snap.player_id = tid;
            EmitTo(st, pushes, map_instance_id, viewer, 2, snap);
            EmitTo(st, pushes, map_instance_id, viewer, 3, snap);
            ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
            {
                const bool inside = tit != st->entities.end() && tit->second.connected &&
                                    InCellRadius(*st, viewer.PresentedX(), viewer.PresentedZ(),
                                                 tit->second.PresentedX(), tit->second.PresentedZ(), radius);
                CountAoiLeave(inside ? AoiLeaveReason::CapEvict : AoiLeaveReason::OutOfWindow, inside);
            }
            RememberLeave(st, pushes, snap, NowMs());
        }
        for (uint64_t tid : neu) {
            if (old.count(tid))
                continue;
            st->watchers[tid].insert(viewer_id);
            if (!emit)
                continue;
            auto tit = st->entities.find(tid);
            if (tit != st->entities.end()) {
                EmitTo(st, pushes, map_instance_id, viewer, 1, tit->second);
                StampEnter(st, pushes, tit->second);
                ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (neu.empty())
            st->visible_targets.erase(viewer_id);
        else
            st->visible_targets[viewer_id] = neu;
        if (mover_id != 0 && mover_id != viewer_id && old.count(mover_id) && neu.count(mover_id)) {
            auto mit = st->entities.find(mover_id);
            if (mit != st->entities.end() &&
                AllowMoveAoi(st, viewer, mit->second, mit->second.last_move_server_ms)) {
                EmitTo(st, pushes, map_instance_id, viewer, 2, mit->second);
                ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ServerStats::aoi_refresh_us_sum.fetch_add(static_cast<uint64_t>(us < 0 ? 0 : us),
                                              std::memory_order_relaxed);
    ServerStats::aoi_refresh_count.fetch_add(1, std::memory_order_relaxed);
    ServerStats::aoi_refresh_viewers_sum.fetch_add(uniq.size(), std::memory_order_relaxed);
}

void MapRuntime::FillSnapshot(const InstanceState &st, uint64_t viewer_id,
                              std::vector<MapEntity> *aoi_snapshot) const {
    if (!aoi_snapshot)
        return;
    aoi_snapshot->clear();
    auto vit = st.visible_targets.find(viewer_id);
    if (vit == st.visible_targets.end())
        return;
    for (uint64_t tid : vit->second) {
        auto it = st.entities.find(tid);
        if (it != st.entities.end() && it->second.connected)
            aoi_snapshot->push_back(it->second);
    }
}

void MapRuntime::RemovePlayerInterest(InstanceState *st, uint64_t map_instance_id,
                                      uint64_t player_id, AoiPushBatch *pushes, bool erase_entity) {
    if (!st)
        return;
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end())
        return;
    const MapEntity gone = eit->second;
    std::vector<uint64_t> viewers = NeighborhoodIds(*st, gone.x, gone.z);
    auto wit = st->watchers.find(player_id);
    if (wit != st->watchers.end())
        viewers.insert(viewers.end(), wit->second.begin(), wit->second.end());
    std::unordered_set<uint64_t> uniq(viewers.begin(), viewers.end());
    for (uint64_t vid : uniq) {
        if (vid == player_id)
            continue;
        auto vit = st->visible_targets.find(vid);
        if (vit == st->visible_targets.end() || vit->second.count(player_id) == 0)
            continue;
        vit->second.erase(player_id);
        if (vit->second.empty())
            st->visible_targets.erase(vit);
        auto rit = st->entities.find(vid);
        if (rit != st->entities.end() && rit->second.connected) {
            EmitTo(st, pushes, map_instance_id, rit->second, 3, gone);
            CountAoiLeave(erase_entity ? AoiLeaveReason::RemoveInterest : AoiLeaveReason::Disconnect,
                          false);
            RememberLeave(st, pushes, gone, NowMs());
        }
    }
    ForgetViewer(st, player_id);
    st->watchers.erase(player_id);
    st->enter_again.erase(player_id);
    for (auto it = st->enter_again.begin(); it != st->enter_again.end();) {
        it->second.erase(player_id);
        if (it->second.empty())
            it = st->enter_again.erase(it);
        else
            ++it;
    }
    std::vector<uint64_t> leave_subjects;
    for (const auto &kv : st->leave_again) {
        if (kv.second.count(player_id) != 0)
            leave_subjects.push_back(kv.first);
    }
    for (uint64_t subject : leave_subjects)
        DropPendingLeave(st, player_id, subject, AoiLeaveDropReason::ViewerOffline);
    st->leave_backoff.erase(player_id);
    AoiDropViewerLeavePending(player_id);
    for (auto it = st->notified.begin(); it != st->notified.end();) {
        if (it->first == player_id) {
            it = st->notified.erase(it);
            continue;
        }
        auto sub = it->second.find(player_id);
        if (sub != it->second.end()) {
            auto rit = st->entities.find(it->first);
            if (rit != st->entities.end() && rit->second.connected) {
                EmitTo(st, pushes, map_instance_id, rit->second, 3, gone);
                CountAoiLeave(erase_entity ? AoiLeaveReason::RemoveInterest : AoiLeaveReason::Disconnect,
                              false);
                RememberLeave(st, pushes, gone, NowMs());
            }
            it->second.erase(sub);
        }
        if (it->second.empty())
            it = st->notified.erase(it);
        else
            ++it;
    }
    st->last_left_ms.erase(player_id);
    for (auto &kv : st->last_left_ms)
        kv.second.erase(player_id);
    RemoveFromCell(st, gone);
    if (erase_entity) {
        st->entities.erase(player_id);
        st->entity_count.fetch_sub(1, std::memory_order_relaxed);
    } else {
        auto left = st->entities.find(player_id);
        if (left != st->entities.end()) {
            left->second.connected = false;
            left->second.disconnected_ms = NowMs();
        }
    }
    if (!AoiV2VisibleSet())
        RefreshViewers(st, viewers, 0, 0, map_instance_id, pushes);
}

bool MapRuntime::AllowMoveAoi(InstanceState *st, const MapEntity &watcher, const MapEntity &subject,
                              int64_t now_ms) const {
    if (!st || watcher.player_id == 0 || subject.player_id == 0)
        return false;
    // 距离用正在显示的位置。权威坐标已经落到路点时，人还在半路上，不能按终点把近处收成远层。
    const float dx = watcher.PresentedX() - subject.PresentedX();
    const float dz = watcher.PresentedZ() - subject.PresentedZ();
    const float dist2 = dx * dx + dz * dz;
    constexpr float kNearM = 16.f;
    const bool far = !(dist2 < kNearM * kNearM);
    auto &rec = st->move_notify_ms[watcher.player_id][subject.player_id];
    const int64_t now = now_ms > 0 ? now_ms : 1;
    if (AoiExtrapolateEnabled()) {
        const bool moving = std::fabs(subject.vx) > 0.05f || std::fabs(subject.vz) > 0.05f;
        bool vel_changed = true;
        bool drifted = false;
        bool keyframe = false;
        if (rec.has) {
            vel_changed = std::fabs(rec.vx - subject.vx) > 0.05f || std::fabs(rec.vz - subject.vz) > 0.05f;
            const float elapsed = static_cast<float>(now - rec.ms) / 1000.f;
            const float pred_x = rec.x + rec.vx * elapsed;
            const float pred_z = rec.z + rec.vz * elapsed;
            const float ex = pred_x - subject.x;
            const float ez = pred_z - subject.z;
            const float err_cm = std::sqrt(ex * ex + ez * ez) * 100.f;
            drifted = err_cm > static_cast<float>(ExtrapErrorCm());
            const int64_t age = now - rec.ms;
            keyframe = moving && !far && age >= ExtrapKeyframeMs();
            if (!vel_changed && !drifted && !keyframe) {
                ServerStats::aoi_move_rate_limited.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (keyframe && !vel_changed && !drifted)
                ServerStats::aoi_keyframe.fetch_add(1, std::memory_order_relaxed);
            NoteMoveInterval(age);
        }
        rec.ms = now;
        rec.x = subject.x;
        rec.z = subject.z;
        rec.vx = subject.vx;
        rec.vz = subject.vz;
        rec.move_start_ms = subject.move_start_ms;
        rec.has = true;
        return true;
    }
    (void)far;
    int64_t gate = 180;
    if (AoiV2Layers()) {
        const float near_m = static_cast<float>(AoiV2NearMeters());
        if (dist2 > near_m * near_m)
            gate = AoiV2FarIntervalMs();
    }
    const float moved_dx = subject.PresentedX() - rec.x;
    const float moved_dz = subject.PresentedZ() - rec.z;
    const bool moved = !rec.has || (moved_dx * moved_dx + moved_dz * moved_dz) > 1e-4f;
    if (!moved) {
        ServerStats::aoi_move_rate_limited.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (rec.has && rec.ms > 0 && now - rec.ms < gate) {
        ServerStats::aoi_move_rate_limited.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (rec.has && rec.ms > 0)
        NoteMoveInterval(now - rec.ms);
    rec.ms = now;
    rec.x = subject.PresentedX();
    rec.z = subject.PresentedZ();
    rec.has = true;
    return true;
}

void MapRuntime::EmitTo(InstanceState *st, AoiPushBatch *pushes, uint64_t map_id,
                        const MapEntity &recipient, int op, const MapEntity &snapshot) {
    if (!pushes || !recipient.connected || recipient.player_id == 0)
        return;
    pushes->map_instance_id = map_id;
    AoiRecipientEvent ev;
    ev.recipient_id = recipient.player_id;
    ev.gateway_instance_id = recipient.gateway_instance_id;
    ev.session_id = recipient.session_id;
    ev.op = op;
    ev.snapshot = snapshot;
    if (ev.snapshot.aoi_live) {
        ev.snapshot.x = ev.snapshot.aoi_x;
        ev.snapshot.z = ev.snapshot.aoi_z;
    }
    // 这一跳的采样时间。不复用移动被接受的时刻，也不能早于该实体上一条 AOI 事件。
    int64_t sample = NowMs();
    if (st && ev.snapshot.player_id != 0) {
        int64_t &last = st->last_aoi_event_ms[ev.snapshot.player_id];
        if (sample <= last)
            sample = last + 1;
        last = sample;
    }
    ev.snapshot.last_move_server_ms = sample;
    if (AoiTraceEnabled()) {
        AoiTraceFields tr;
        tr.viewer = ev.recipient_id;
        tr.subject = ev.snapshot.player_id;
        tr.op = ev.op;
        tr.version = ev.notify_version != 0 ? ev.notify_version : ev.snapshot.state_seq;
        tr.offer_ms = sample;
        AoiTrace(tr);
    }
    pushes->events.push_back(std::move(ev));
}

bool MapRuntime::Enter(uint64_t map_instance_id, std::shared_ptr<const MapStaticData> data,
                       MapEntity entity, MapEntity *self_out, std::vector<MapEntity> *aoi_snapshot,
                       AoiPushBatch *pushes, std::string *err, int view_radius_cells,
                       int max_visible) {
    if (map_instance_id == 0 || entity.player_id == 0 || !data) {
        if (err)
            *err = "invalid enter";
        return false;
    }
    if (!Finite4(entity.x, entity.y, entity.z, entity.yaw)) {
        if (err)
            *err = "ERR_INVALID_POSITION";
        return false;
    }
    std::shared_ptr<InstanceState> st;
    std::shared_ptr<InstanceState> old;
    uint64_t old_id = 0;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto &slot = maps_[map_instance_id];
        if (!slot) {
            slot = std::make_shared<InstanceState>();
            slot->data = data;
            int radius = view_radius_cells >= 0 ? view_radius_cells : view_radius_;
            const int over = AoiV2RadiusOverride();
            if (over >= 0)
                radius = over;
            slot->view_radius = radius;
            int cap = max_visible >= 0 ? max_visible : max_visible_;
            if (cap < 1)
                cap = 1;
            if (cap > 200)
                cap = 200;
            slot->max_visible = cap;
            slot->epoch = next_epoch_++;
        } else if (!slot->data) {
            slot->data = data;
            if (slot->view_radius < 0)
                slot->view_radius = view_radius_cells >= 0 ? view_radius_cells : view_radius_;
        }
        st = slot;
        auto pit = player_map_.find(entity.player_id);
        if (pit != player_map_.end() && pit->second != map_instance_id) {
            old_id = pit->second;
            auto oit = maps_.find(old_id);
            if (oit != maps_.end())
                old = oit->second;
        }
    }
    InstLock lock_a;
    InstLock lock_b;
    if (old && old_id != map_instance_id) {
        if (old_id < map_instance_id) {
            lock_a = LockInst(old);
            lock_b = LockInst(st);
        } else {
            lock_a = LockInst(st);
            lock_b = LockInst(old);
        }
    } else {
        lock_a = LockInst(st);
    }
    if (!st || st->unloaded) {
        if (err)
            *err = "map unloaded";
        return false;
    }
    bool old_empty = false;
    if (old && old_id != map_instance_id && !old->unloaded) {
        RemovePlayerInterest(old.get(), old_id, entity.player_id, pushes, true);
        old_empty = old->entity_count.load(std::memory_order_relaxed) == 0;
    }
    auto existing = st->entities.find(entity.player_id);
    if (existing != st->entities.end()) {
        entity.state_seq = existing->second.state_seq;
        entity.last_client_seq = 0;
        entity.last_move_server_ms = existing->second.last_move_server_ms;
        entity.last_client_time_ms = existing->second.last_client_time_ms;
        if (entity.x == 0 && entity.y == 0 && entity.z == 0) {
            entity.x = existing->second.x;
            entity.y = existing->second.y;
            entity.z = existing->second.z;
            entity.yaw = existing->second.yaw;
        }
        RemovePlayerInterest(st.get(), map_instance_id, entity.player_id, pushes, true);
    }
    entity.connected = true;
    entity.disconnected_ms = 0;
    if (entity.state_seq == 0)
        entity.state_seq = 1;
    st->entities[entity.player_id] = entity;
    st->entity_count.fetch_add(1, std::memory_order_relaxed);
    PlaceInCell(st.get(), entity);
    if (AoiV2VisibleSet()) {
        auto &self = st->entities[entity.player_id];
        SyncVisibleSubject(st.get(), self, NowMs(), map_instance_id, pushes);
        const std::vector<uint64_t> around = NeighborhoodIds(*st, entity.x, entity.z);
        for (uint64_t id : around) {
            if (id == entity.player_id)
                continue;
            auto it = st->entities.find(id);
            if (it == st->entities.end())
                continue;
            SyncVisibleSubject(st.get(), it->second, NowMs(), map_instance_id, pushes);
        }
    } else {
        std::vector<uint64_t> viewers = NeighborhoodIds(*st, entity.x, entity.z);
        viewers.push_back(entity.player_id);
        RefreshViewers(st.get(), viewers, 0, entity.player_id, map_instance_id, pushes);
    }
    FillSnapshot(*st, entity.player_id, aoi_snapshot);
    if (self_out)
        *self_out = entity;
    if (lock_a.owns_lock())
        lock_a.unlock();
    if (lock_b.owns_lock())
        lock_b.unlock();
    if (old_id != 0) {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(entity.player_id);
        if (pit != player_map_.end() && pit->second == old_id)
            player_map_.erase(pit);
    }
    if (old_empty)
        RetireIfEmpty(old_id, old);
    if (!PublishPlayer(entity.player_id, map_instance_id, st)) {
        {
            auto rollback = LockInst(st);
            if (st->entities.count(entity.player_id) != 0)
                RemovePlayerInterest(st.get(), map_instance_id, entity.player_id, pushes, true);
        }
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(entity.player_id);
        if (pit != player_map_.end() && pit->second == map_instance_id)
            player_map_.erase(pit);
        if (err)
            *err = "map unloaded";
        return false;
    }
    return true;
}

MapMoveReject MapRuntime::Move(uint64_t map_instance_id, uint64_t player_id, float x, float y,
                               float z, float yaw, uint64_t client_seq, int64_t now_ms,
                               MapEntity *confirmed, AoiPushBatch *pushes, std::string *err_code,
                               bool ignore_speed, int64_t client_time_ms) {
    auto fail = [&](MapMoveReject r, const char *code) {
        if (err_code)
            *err_code = code;
        return r;
    };
    if (!Finite4(x, y, z, yaw))
        return fail(MapMoveReject::NotFinite, "ERR_INVALID_POSITION");
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto mit = maps_.find(map_instance_id);
        if (mit == maps_.end() || !mit->second)
            return fail(MapMoveReject::NotOnMap, "ERR_NOT_ON_MAP");
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return fail(MapMoveReject::NotOnMap, "ERR_NOT_ON_MAP");
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end())
        return fail(MapMoveReject::NotOnMap, "ERR_NOT_ON_MAP");
    MapEntity &e = eit->second;
    if (!e.connected)
        return fail(MapMoveReject::Disconnected, "ERR_DISCONNECTED");
    if (client_seq != 0 && e.last_client_seq != 0 && client_seq <= e.last_client_seq) {
        const uint64_t expected_seq_min = e.last_client_seq;
        ServerStats::move_err_stale_seq.fetch_add(1, std::memory_order_relaxed);
        SetLogPlayer(player_id);
        SetLogErrorCode("ERR_STALE_SEQ");
        LOG_WARN << "ERR_STALE_SEQ expected_seq_min=" << expected_seq_min
                 << " received_seq=" << client_seq << " player_id=" << player_id;
        SetLogErrorCode("");
        return fail(MapMoveReject::StaleSeq, "ERR_STALE_SEQ");
    }
    if (!st->data)
        return fail(MapMoveReject::NotOnMap, "ERR_MAP_RUNTIME_NOT_READY");
    // 贴着 Y 盒底的是被确认过的坠落，不是地表。先抬回出生高度，拒绝回包才不会停在盒子底。
    if (e.y <= st->data->bounds_min().y + 2.f)
        e.y = st->data->default_spawn().position.y;
    float use_y = y;
    if (!st->data->InBounds(x, use_y, z)) {
        if (st->data->InBounds(x, e.y, z))
            use_y = e.y;
        else
            return fail(MapMoveReject::OutOfBounds, "ERR_OUT_OF_BOUNDS");
    }
    float want_x = x, want_y = use_y, want_z = z;
    // 几乎没有水平位移时，不允许把高度掉下去。沿路走可以按坡度下降。
    if (!ignore_speed) {
        const float hx = want_x - e.x;
        const float hz = want_z - e.z;
        const float horiz = std::sqrt(hx * hx + hz * hz);
        const float max_drop = horiz < 0.5f ? 0.4f : horiz + 0.5f;
        if (want_y < e.y - max_drop)
            want_y = e.y - max_drop;
    }
    const float dx = want_x - e.x, dy = want_y - e.y, dz = want_z - e.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    // 统计：与权威坐标几乎重合的重复目标（含 2s 超时重发）。只计数，本轮不改发送路径。
    if (dist < 0.05f)
        AoiNoteDuplicateMoveTarget();
    const int64_t t1 = now_ms > 0 ? now_ms : NowMs();
    float dt_server = 0.f;
    if (e.last_move_server_ms > 0 && t1 > e.last_move_server_ms)
        dt_server = static_cast<float>(t1 - e.last_move_server_ms) / 1000.f;
    float dt_client = 0.f;
    if (client_time_ms > 0 && e.last_client_time_ms > 0 && client_time_ms > e.last_client_time_ms)
        dt_client = static_cast<float>(client_time_ms - e.last_client_time_ms) / 1000.f;
    // 有客户端时间就用客户端时间。两边都没有历史时，不能套 1 秒：稀疏巡逻的第一段是 10 到 30 秒的路点，
    // 收成 1 秒后身体很快走完就停，客户端还要等整段结束才发下一点。
    float dt = dt_client > 0.f ? dt_client : dt_server;
    if (!(dt > 0.f))
        dt = 30.f;
    const float speed = e.move_speed > 0.f ? e.move_speed : 10.f;
    const bool within_speed = dt > 1e-4f && dist <= speed * dt + 1e-3f;
    const float max_dist = speed * dt * 1.25f + 0.5f;
    bool walked_slide = false;
    // 8 米以上是一整段路点。稀疏巡逻在上一段结束 50 毫秒后就发下一段终点，
    // 按这个时间差收成 1 米后，身体马上走完，客户端还要等 10 到 30 秒。
    // 整段照原样接受，呈现位置仍按 move_speed 走，不会瞬移。短步超速才缩短。
    const bool route = dist >= 8.f;
    bool clamped = false;
    if (!ignore_speed && !route && !within_speed && dist > max_dist && dist > 1e-6f) {
        clamped = true;
        const float scale = max_dist / dist;
        want_x = e.x + dx * scale;
        want_y = e.y + dy * scale;
        want_z = e.z + dz * scale;
        if (!st->data->InBounds(want_x, want_y, want_z))
            return fail(MapMoveReject::TooFast, "ERR_MOVE_TOO_FAST");
        if (!st->data->IsWalkable(want_x, want_z)) {
            float sx = want_x, sz = want_z;
            if (!st->data->ClampOntoWalkable(e.x, e.z, want_x, want_z, &sx, &sz))
                return fail(MapMoveReject::TooFast, "ERR_MOVE_TOO_FAST");
            want_x = sx;
            want_z = sz;
            walked_slide = true;
        }
        ServerStats::move_speed_clamp.fetch_add(1, std::memory_order_relaxed);
    }
    if (walked_slide)
        ServerStats::move_walk_slide.fetch_add(1, std::memory_order_relaxed);

    const float old_x = e.PresentedX();
    const float old_z = e.PresentedZ();
    int old_cx = 0, old_cz = 0, new_cx = 0, new_cz = 0;
    st->data->WorldToAoiCell(old_x, old_z, &old_cx, &old_cz);
    const float step_dx = want_x - old_x;
    const float step_dz = want_z - old_z;
    const float step_dist = std::sqrt(step_dx * step_dx + step_dz * step_dz);
    // 不到半米直接落到目标格。再远的步子留在现在的格子，按 move_speed 走过去。
    // 以前 8 米才走，短步和被收短的步都会在 AOI 里瞬移一次然后站着。
    const bool glide = step_dist >= 0.4f;
    if (glide)
        st->data->WorldToAoiCell(old_x, old_z, &new_cx, &new_cz);
    else
        st->data->WorldToAoiCell(want_x, want_z, &new_cx, &new_cz);
    RemoveFromCell(st.get(), e);
    e.x = want_x;
    e.y = want_y;
    e.z = want_z;
    e.yaw = yaw;
    e.state_seq += 1;
    if (client_seq != 0)
        e.last_client_seq = client_seq;
    e.last_move_server_ms = t1;
    if (client_time_ms > 0)
        e.last_client_time_ms = client_time_ms;
    const bool displaced = !(old_x == want_x && old_z == want_z);
    if (displaced)
        e.last_displace_ms = t1;
    if (!glide) {
        e.aoi_x = want_x;
        e.aoi_z = want_z;
        e.aoi_live = true;
    } else if (!e.aoi_live) {
        e.aoi_x = old_x;
        e.aoi_z = old_z;
        e.aoi_live = true;
    }
    float nvx = 0.f;
    float nvz = 0.f;
    if (displaced && step_dist > 1e-4f) {
        // 呈现速度封顶在 move_speed。收包间隔只有几十毫秒时，用它做分母会在一帧里走到终点。
        const float cap = e.move_speed > 0.f ? e.move_speed : 10.f;
        nvx = step_dx / step_dist * cap;
        nvz = step_dz / step_dist * cap;
    }
    const bool vel_changed = std::fabs(nvx - e.vx) > 0.05f || std::fabs(nvz - e.vz) > 0.05f;
    e.vx = nvx;
    e.vz = nvz;
    if (vel_changed || e.move_start_ms == 0)
        e.move_start_ms = t1;
    PlaceInCell(st.get(), e);
    const bool same_cell = old_cx == new_cx && old_cz == new_cz;
    // 格子占用版本没变时，成员关系和上次算的一样。不用再扫邻居，Move 持锁是 O(1)。
    // 有人进出格子才会 ++cell_epoch，下一次再重算。
    std::vector<uint64_t> neigh;
    bool membership_stable = false;
    auto self_vis = st->visible_targets.find(player_id);
    if (!AoiMembershipUnified() && same_cell && e.membership_epoch != 0 &&
        e.membership_epoch == st->cell_epoch && self_vis != st->visible_targets.end()) {
        membership_stable = true;
    } else if (!AoiMembershipUnified()) {
    neigh = NeighborhoodIds(*st, e.PresentedX(), e.PresentedZ());
    membership_stable = same_cell && neigh.size() <= static_cast<size_t>(st->max_visible) + 1 &&
                        self_vis != st->visible_targets.end();
    if (membership_stable) {
        for (uint64_t pid : neigh) {
            if (pid == player_id)
                continue;
            if (self_vis->second.count(pid) == 0) {
                membership_stable = false;
                break;
            }
        }
        if (self_vis->second.size() + 1 != neigh.size())
            membership_stable = false;
    }
    if (membership_stable)
        e.membership_epoch = st->cell_epoch;
    }
    bool emitted_fast = false;
    if (!displaced) {
        // 坐标没变就不再给观察者发周期位置。自己的 Move 回包仍然带着权威坐标。
        ServerStats::aoi_stationary_skip.fetch_add(1, std::memory_order_relaxed);
        emitted_fast = true;
    }
    if (membership_stable && displaced) {
        auto wit = st->watchers.find(player_id);
        if (wit != st->watchers.end() && !wit->second.empty()) {
            emitted_fast = true;
            for (uint64_t vid : wit->second) {
                auto rit = st->entities.find(vid);
                if (rit == st->entities.end() || !rit->second.connected)
                    continue;
                if (!AllowMoveAoi(st.get(), rit->second, e, t1))
                    continue;
                EmitTo(st.get(), pushes, map_instance_id, rit->second, 2, e);
                ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
                ServerStats::aoi_fast_move.fetch_add(1, std::memory_order_relaxed);
                emitted_fast = true;
            }
        }
    }
    if (!AoiV2VisibleSet() && !emitted_fast) {
        std::vector<uint64_t> viewers = NeighborhoodIds(*st, old_x, old_z);
        viewers.insert(viewers.end(), neigh.begin(), neigh.end());
        auto wit = st->watchers.find(player_id);
        if (wit != st->watchers.end())
            viewers.insert(viewers.end(), wit->second.begin(), wit->second.end());
        viewers.push_back(player_id);
        RefreshViewers(st.get(), viewers, player_id, 0, map_instance_id, pushes);
    }
    if (AoiV2VisibleSet() && displaced)
        SyncVisibleSubject(st.get(), e, t1, map_instance_id, pushes);
    int watchers = 0;
    auto trace_wit = st->watchers.find(player_id);
    if (trace_wit != st->watchers.end())
        watchers = static_cast<int>(trace_wit->second.size());
    LogMoveTrace(displaced ? "accept" : "unchanged", e, old_x, old_z, step_dist, dt, watchers, glide,
                 route, clamped);
    if (confirmed)
        *confirmed = e;
    return MapMoveReject::Ok;
}

bool MapRuntime::Leave(uint64_t map_instance_id, uint64_t player_id, AoiPushBatch *pushes) {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto mit = maps_.find(map_instance_id);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    bool empty = false;
    {
        auto inst = LockInst(st);
        if (st->unloaded || st->entities.count(player_id) == 0)
            return false;
        RemovePlayerInterest(st.get(), map_instance_id, player_id, pushes, true);
        empty = st->entity_count.load(std::memory_order_relaxed) == 0;
    }
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit != player_map_.end() && pit->second == map_instance_id)
            player_map_.erase(pit);
    }
    if (empty)
        RetireIfEmpty(map_instance_id, st);
    return true;
}

size_t MapRuntime::DropExcept(uint64_t map_instance_id, const std::unordered_set<uint64_t> &keep,
                              AoiPushBatch *pushes) {
    std::vector<uint64_t> drop;
    {
        std::shared_ptr<InstanceState> st;
        {
            std::lock_guard<std::mutex> reg(mu_);
            auto mit = maps_.find(map_instance_id);
            if (mit == maps_.end() || !mit->second)
                return 0;
            st = mit->second;
        }
        auto inst = LockInst(st);
        if (st->unloaded)
            return 0;
        drop.reserve(st->entities.size());
        for (const auto &kv : st->entities) {
            if (keep.count(kv.first) == 0)
                drop.push_back(kv.first);
        }
    }
    size_t n = 0;
    for (uint64_t pid : drop) {
        if (Leave(map_instance_id, pid, pushes))
            ++n;
    }
    return n;
}

void MapRuntime::LeaveAll(uint64_t player_id, AoiPushBatch *pushes) {
    uint64_t mid = 0;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end())
            return;
        mid = pit->second;
        auto mit = maps_.find(mid);
        if (mit == maps_.end() || !mit->second) {
            player_map_.erase(pit);
            return;
        }
        st = mit->second;
    }
    bool empty = false;
    {
        auto inst = LockInst(st);
        if (st->unloaded || st->entities.count(player_id) == 0)
            return;
        RemovePlayerInterest(st.get(), mid, player_id, pushes, true);
        empty = st->entity_count.load(std::memory_order_relaxed) == 0;
    }
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit != player_map_.end() && pit->second == mid)
            player_map_.erase(pit);
    }
    if (empty)
        RetireIfEmpty(mid, st);
}

size_t MapRuntime::ExpireDisconnected(int64_t grace_ms, AoiPushBatch *pushes) {
    if (grace_ms < 1000)
        grace_ms = 1000;
    const int64_t now = NowMs();
    std::vector<std::shared_ptr<InstanceState>> maps;
    {
        std::lock_guard<std::mutex> reg(mu_);
        maps.reserve(maps_.size());
        for (const auto &mk : maps_) {
            if (mk.second)
                maps.push_back(mk.second);
        }
    }
    std::vector<uint64_t> due;
    for (const auto &st : maps) {
        auto inst = LockInst(st);
        for (const auto &ek : st->entities) {
            const MapEntity &e = ek.second;
            if (e.connected || e.disconnected_ms <= 0 || now - e.disconnected_ms < grace_ms)
                continue;
            due.push_back(e.player_id);
        }
    }
    size_t n = 0;
    for (uint64_t pid : due) {
        uint64_t mid = 0;
        std::shared_ptr<InstanceState> st;
        {
            std::lock_guard<std::mutex> reg(mu_);
            auto pit = player_map_.find(pid);
            if (pit == player_map_.end())
                continue;
            mid = pit->second;
            auto mit = maps_.find(mid);
            if (mit == maps_.end() || !mit->second)
                continue;
            st = mit->second;
        }
        bool erase = false;
        {
            auto inst = LockInst(st);
            if (st->unloaded)
                continue;
            auto eit = st->entities.find(pid);
            if (eit == st->entities.end() || eit->second.connected || eit->second.disconnected_ms <= 0 ||
                now - eit->second.disconnected_ms < grace_ms)
                continue;
            const MapEntity gone = eit->second;
            const std::vector<uint64_t> viewers = NeighborhoodIds(*st, gone.x, gone.z);
            for (uint64_t vid : viewers) {
                if (vid == pid)
                    continue;
                auto rit = st->entities.find(vid);
                if (rit != st->entities.end() && rit->second.connected) {
                    EmitTo(st.get(), pushes, mid, rit->second, 3, gone);
                    CountAoiLeave(AoiLeaveReason::Disconnect, false);
                }
            }
            RemovePlayerInterest(st.get(), mid, pid, pushes, true);
            erase = st->entity_count.load(std::memory_order_relaxed) == 0;
        }
        {
            std::lock_guard<std::mutex> reg(mu_);
            auto pit = player_map_.find(pid);
            if (pit != player_map_.end() && pit->second == mid)
                player_map_.erase(pit);
        }
        if (erase)
            RetireIfEmpty(mid, st);
        ++n;
    }
    return n;
}

bool MapRuntime::Disconnect(uint64_t player_id, AoiPushBatch *pushes) {
    // 必须对邻居发 LEAVE：否则客户端 AOI 模型永不消失（静默断线已在实机压测验证为幽灵模型根因）。
    // entity 保留（erase_entity=false）以便宽限内 Reconnect；LEAVE 走 reliable 推送。
    uint64_t mid = 0;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end())
            return false;
        mid = pit->second;
        auto mit = maps_.find(mid);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return false;
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end())
        return false;
    if (!eit->second.connected)
        return true;
    RemovePlayerInterest(st.get(), mid, player_id, pushes, false);
    return true;
}

bool MapRuntime::Reconnect(uint64_t player_id, const std::string &gateway_instance_id,
                           const std::string &session_id, MapEntity *self_out,
                           std::vector<MapEntity> *aoi_snapshot, AoiPushBatch *pushes) {
    uint64_t mid = 0;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end())
            return false;
        mid = pit->second;
        auto mit = maps_.find(mid);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return false;
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end())
        return false;
    MapEntity &e = eit->second;
    e.connected = true;
    e.disconnected_ms = 0;
    if (!gateway_instance_id.empty())
        e.gateway_instance_id = gateway_instance_id;
    if (!session_id.empty())
        e.session_id = session_id;
    PlaceInCell(st.get(), e);
    // 重连者本人：静默建可见集 + 快照 ENTER。
    // 邻居：RefreshViewers 发 ENTER（断线已 LEAVE，必须补回；可丢弃推送靠 TrySend 背压）。
    std::vector<uint64_t> viewers = NeighborhoodIds(*st, e.x, e.z);
    viewers.push_back(player_id);
    RefreshViewers(st.get(), viewers, 0, player_id, mid, pushes);
    FillSnapshot(*st, player_id, aoi_snapshot);
    if (pushes && aoi_snapshot) {
        for (const auto &other : *aoi_snapshot) {
            EmitTo(st.get(), pushes, mid, e, 1, other);
            StampEnter(st.get(), pushes, other);
        }
    }
    if (self_out)
        *self_out = e;
    return true;
}

bool MapRuntime::HasPlayer(uint64_t map_instance_id, uint64_t player_id) const {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto mit = maps_.find(map_instance_id);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return false;
    return st->entities.count(player_id) != 0;
}

void MapRuntime::CollectXz(uint64_t map_instance_id, uint64_t except_player,
                           std::vector<std::pair<float, float>> *out) const {
    if (out)
        out->clear();
    if (!out || map_instance_id == 0)
        return;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto mit = maps_.find(map_instance_id);
        if (mit == maps_.end() || !mit->second)
            return;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return;
    out->reserve(st->entities.size());
    for (const auto &kv : st->entities) {
        if (kv.first == except_player)
            continue;
        out->emplace_back(kv.second.x, kv.second.z);
    }
}

uint32_t MapRuntime::ConnectedCount(uint64_t map_instance_id) const {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto mit = maps_.find(map_instance_id);
        if (mit == maps_.end() || !mit->second)
            return 0;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return 0;
    uint32_t n = 0;
    for (const auto &kv : st->entities) {
        if (kv.second.connected)
            ++n;
    }
    return n;
}

uint64_t MapRuntime::PlayerMap(uint64_t player_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = player_map_.find(player_id);
    return it == player_map_.end() ? 0 : it->second;
}

bool MapRuntime::CollectFullView(uint64_t player_id, FullView *out) const {
    if (out)
        *out = FullView{};
    if (!out || player_id == 0)
        return false;
    out->radius_m = 32.f;
    uint64_t mid = 0;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end()) {
            out->step = "not_on_map";
            return true;
        }
        mid = pit->second;
        auto mit = maps_.find(mid);
        if (mit == maps_.end() || !mit->second) {
            out->step = "not_on_map";
            return true;
        }
        st = mit->second;
    }
    auto inst = LockInst(st);
    out->map_instance_id = mid;
    if (st->unloaded || !st->data) {
        out->step = "no_map_data";
        return true;
    }
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end()) {
        out->step = "not_on_map";
        return true;
    }
    out->self = eit->second;
    out->cell_m = st->data->aoi_cell_size();
    out->radius_cells = ResolvedRadius(*st);
    auto cit = st->visible_targets.find(player_id);
    out->cache_n = cit == st->visible_targets.end() ? 0 : cit->second.size();
    // 断线后实体还在，但已经从格子里摘掉，推送兴趣也不再维护。
    // 这时缓存一定是空的；回 ok 加空列表会让客户端把还在画面里的人删掉。
    if (!eit->second.connected) {
        out->step = "viewer_not_online";
        return true;
    }
    const auto ids = VisibleIds(*st, eit->second);
    struct Near {
        uint64_t id = 0;
        float d2 = 0.f;
        MapEntity entity;
    };
    std::vector<Near> picked;
    picked.reserve(ids.size() + 8);
    auto consider = [&](const MapEntity &other) {
        if (other.player_id == player_id || other.player_id == 0 || !other.connected)
            return;
        const float dx = other.PresentedX() - eit->second.PresentedX();
        const float dz = other.PresentedZ() - eit->second.PresentedZ();
        const float d2 = dx * dx + dz * dz;
        const bool in_push = ids.count(other.player_id) != 0;
        const bool within_32 = d2 <= out->radius_m * out->radius_m;
        if (!in_push && !within_32)
            return;
        for (const auto &n : picked) {
            if (n.id == other.player_id)
                return;
        }
        MapEntity shown = other;
        if (shown.aoi_live) {
            shown.x = shown.aoi_x;
            shown.z = shown.aoi_z;
        }
        picked.push_back(Near{other.player_id, d2, shown});
    };
    for (uint64_t tid : ids) {
        auto tit = st->entities.find(tid);
        if (tit != st->entities.end())
            consider(tit->second);
    }
    for (const auto &kv : st->entities)
        consider(kv.second);
    const size_t cap = static_cast<size_t>(st->max_visible > 0 ? st->max_visible : max_visible_);
    if (picked.size() > cap) {
        std::nth_element(picked.begin(), picked.begin() + static_cast<std::ptrdiff_t>(cap),
                         picked.end(), [](const Near &a, const Near &b) {
                             if (a.d2 < b.d2)
                                 return true;
                             if (b.d2 < a.d2)
                                 return false;
                             return a.id < b.id;
                         });
        picked.resize(cap);
    }
    out->interest.reserve(picked.size());
    for (const auto &n : picked)
        out->interest.push_back(n.entity);
    // 格子里还有在线的人，但兴趣集是空的：不能回 ok 加空列表。客户端会按全量替换，
    // 把画面上的人删掉。让它重试，而不是把「周围有人」写成空视野。
    if (out->interest.empty()) {
        // 第一遍漏掉的在线邻居也要补上。周围有人时不能回空表，快照会把人拉回出生点或删掉。
        for (const auto &kv : st->entities) {
            if (kv.first == player_id || !kv.second.connected)
                continue;
            const float dx = kv.second.PresentedX() - eit->second.PresentedX();
            const float dz = kv.second.PresentedZ() - eit->second.PresentedZ();
            if (dx * dx + dz * dz > out->radius_m * out->radius_m)
                continue;
            MapEntity shown = kv.second;
            if (shown.aoi_live) {
                shown.x = shown.aoi_x;
                shown.z = shown.aoi_z;
            }
            out->interest.push_back(shown);
        }
    }
    if (out->interest.empty()) {
        const auto neigh = NeighborhoodIds(*st, eit->second.PresentedX(), eit->second.PresentedZ());
        for (uint64_t id : neigh) {
            if (id == player_id)
                continue;
            auto nit = st->entities.find(id);
            if (nit != st->entities.end() && nit->second.connected) {
                out->ready = false;
                out->step = "view_inconsistent";
                return true;
            }
        }
    }
    out->ready = true;
    out->step = out->interest.empty() ? "view_empty" : "interest_live";
    return true;
}

bool MapRuntime::SnapshotView(uint64_t player_id, MapEntity *self_out,
                              std::vector<MapEntity> *aoi_snapshot,
                              uint64_t *map_instance_id) const {
    uint64_t mid = 0;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end())
            return false;
        mid = pit->second;
        auto mit = maps_.find(mid);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    auto inst = LockInst(st);
    if (st->unloaded)
        return false;
    auto eit = st->entities.find(player_id);
    if (eit == st->entities.end())
        return false;
    if (map_instance_id)
        *map_instance_id = mid;
    if (self_out)
        *self_out = eit->second;
    if (!eit->second.connected) {
        if (aoi_snapshot)
            aoi_snapshot->clear();
        return true;
    }
    FillSnapshot(*st, player_id, aoi_snapshot);
    return true;
}

void MapRuntime::CopyVisibleSets(std::vector<std::pair<uint64_t, std::vector<uint64_t>>> *out) const {
    if (!out)
        return;
    out->clear();
    std::vector<std::shared_ptr<InstanceState>> maps;
    {
        std::lock_guard<std::mutex> reg(mu_);
        maps.reserve(maps_.size());
        for (const auto &kv : maps_) {
            if (kv.second)
                maps.push_back(kv.second);
        }
    }
    for (const auto &st : maps) {
        auto inst = LockInst(st);
        if (!st || st->unloaded)
            continue;
        for (const auto &kv : st->visible_targets) {
            std::vector<uint64_t> ids(kv.second.begin(), kv.second.end());
            std::sort(ids.begin(), ids.end());
            out->emplace_back(kv.first, std::move(ids));
        }
    }
}

int MapRuntime::TotalEntityCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (const auto &kv : maps_) {
        if (kv.second)
            n += kv.second->entity_count.load(std::memory_order_relaxed);
    }
    return n;
}

bool MapRuntime::ViewerConnected(uint64_t player_id) const {
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto pit = player_map_.find(player_id);
        if (pit == player_map_.end())
            return false;
        auto mit = maps_.find(pit->second);
        if (mit == maps_.end() || !mit->second)
            return false;
        st = mit->second;
    }
    auto inst = LockInst(st);
    auto eit = st->entities.find(player_id);
    return eit != st->entities.end() && eit->second.connected;
}

std::vector<uint64_t> MapRuntime::ListPlayers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> out;
    out.reserve(player_map_.size());
    for (const auto &kv : player_map_)
        out.push_back(kv.first);
    return out;
}

void MapRuntime::ClearForTest() {
    SetNowMsForTest(0);
    std::vector<std::shared_ptr<InstanceState>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : maps_)
            all.push_back(kv.second);
        maps_.clear();
        player_map_.clear();
    }
    for (auto &st : all) {
        if (!st)
            continue;
        auto inst = LockInst(st);
        st->unloaded = true;
        st->entities.clear();
        st->cells.clear();
        st->visible_targets.clear();
        st->watchers.clear();
        {
            std::vector<std::pair<uint64_t, uint64_t>> queued;
            for (const auto &sk : st->leave_again)
                for (const auto &vk : sk.second)
                    queued.emplace_back(vk.first, sk.first);
            for (const auto &row : queued)
                DropPendingLeave(st.get(), row.first, row.second, AoiLeaveDropReason::ViewerOffline);
        }
        st->leave_again.clear();
        st->leave_backoff.clear();
        st->enter_again.clear();
        st->notified.clear();
        st->last_left_ms.clear();
        st->entity_count.store(0, std::memory_order_relaxed);
    }
}

bool MapRuntime::Unload(uint64_t map_instance_id) {
    if (map_instance_id == 0)
        return false;
    std::shared_ptr<InstanceState> st;
    {
        std::lock_guard<std::mutex> reg(mu_);
        auto it = maps_.find(map_instance_id);
        if (it == maps_.end() || !it->second)
            return false;
        st = it->second;
        maps_.erase(it);
    }
    std::vector<uint64_t> ids;
    {
        auto inst = LockInst(st);
        st->unloaded = true;
        ids.reserve(st->entities.size());
        for (const auto &kv : st->entities)
            ids.push_back(kv.first);
        st->entities.clear();
        st->cells.clear();
        st->visible_targets.clear();
        st->watchers.clear();
        {
            std::vector<std::pair<uint64_t, uint64_t>> queued;
            for (const auto &sk : st->leave_again)
                for (const auto &vk : sk.second)
                    queued.emplace_back(vk.first, sk.first);
            for (const auto &row : queued)
                DropPendingLeave(st.get(), row.first, row.second, AoiLeaveDropReason::ViewerOffline);
        }
        st->leave_again.clear();
        st->leave_backoff.clear();
        st->enter_again.clear();
        st->notified.clear();
        st->last_left_ms.clear();
        st->entity_count.store(0, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> reg(mu_);
    for (uint64_t id : ids) {
        auto pit = player_map_.find(id);
        if (pit != player_map_.end() && pit->second == map_instance_id)
            player_map_.erase(pit);
    }
    return true;
}
