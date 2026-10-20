#include "MapRuntime.h"

#include "Logging.h"
#include "ServerStats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace {

bool Finite4(float a, float b, float c, float d) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
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

MapRuntime &MapRuntime::Instance() {
    static MapRuntime g;
    return g;
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

std::unique_lock<std::mutex> MapRuntime::LockInst(const std::shared_ptr<InstanceState> &st) const {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(st->mu);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ServerStats::aoi_lock_wait_us_sum.fetch_add(static_cast<uint64_t>(us < 0 ? 0 : us),
                                                std::memory_order_relaxed);
    ServerStats::aoi_lock_samples.fetch_add(1, std::memory_order_relaxed);
    return lk;
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

int64_t MapRuntime::NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void MapRuntime::PlaceInCell(InstanceState *st, const MapEntity &e) {
    if (!st || !st->data)
        return;
    int cx = 0, cz = 0;
    st->data->WorldToAoiCell(e.x, e.z, &cx, &cz);
    st->cells[CellKey(cx, cz)].insert(e.player_id);
}

void MapRuntime::RemoveFromCell(InstanceState *st, const MapEntity &e) {
    if (!st || !st->data)
        return;
    int cx = 0, cz = 0;
    st->data->WorldToAoiCell(e.x, e.z, &cx, &cz);
    auto it = st->cells.find(CellKey(cx, cz));
    if (it == st->cells.end())
        return;
    it->second.erase(e.player_id);
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
    st.data->WorldToAoiCell(e.x, e.z, &cx, &cz);
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
                const float ddx = eit->second.x - e.x;
                const float ddz = eit->second.z - e.z;
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

std::vector<uint64_t> MapRuntime::NeighborhoodIds(const InstanceState &st, float x, float z) const {
    std::vector<uint64_t> ids;
    if (!st.data)
        return ids;
    const int r = ResolvedRadius(st);
    if (r < 0)
        return ids;
    int cx = 0, cz = 0;
    st.data->WorldToAoiCell(x, z, &cx, &cz);
    // r == 0 只包含当前格子，与 VisibleIds 相同。
    for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) {
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

void MapRuntime::ForgetViewer(InstanceState *st, uint64_t viewer_id) {
    if (!st)
        return;
    auto it = st->visible_targets.find(viewer_id);
    if (it == st->visible_targets.end())
        return;
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
        const auto neu = VisibleIds(*st, viewer);
        const bool emit = viewer_id != silent_viewer;
        for (uint64_t tid : neu) {
            if (old.count(tid))
                continue;
            st->watchers[tid].insert(viewer_id);
            if (!emit)
                continue;
            auto tit = st->entities.find(tid);
            if (tit != st->entities.end()) {
                EmitTo(pushes, map_instance_id, viewer, 1, tit->second);
                ServerStats::aoi_sub_enter.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (uint64_t tid : old) {
            if (neu.count(tid))
                continue;
            auto w = st->watchers.find(tid);
            if (w != st->watchers.end()) {
                w->second.erase(viewer_id);
                if (w->second.empty())
                    st->watchers.erase(w);
            }
            if (!emit)
                continue;
            auto tit = st->entities.find(tid);
            MapEntity snap;
            if (tit != st->entities.end())
                snap = tit->second;
            else
                snap.player_id = tid;
            EmitTo(pushes, map_instance_id, viewer, 3, snap);
            ServerStats::aoi_sub_leave.fetch_add(1, std::memory_order_relaxed);
        }
        if (neu.empty())
            st->visible_targets.erase(viewer_id);
        else
            st->visible_targets[viewer_id] = neu;
        if (mover_id != 0 && mover_id != viewer_id && old.count(mover_id) && neu.count(mover_id)) {
            auto mit = st->entities.find(mover_id);
            if (mit != st->entities.end()) {
                EmitTo(pushes, map_instance_id, viewer, 2, mit->second);
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
        if (rit != st->entities.end() && rit->second.connected)
            EmitTo(pushes, map_instance_id, rit->second, 3, gone);
    }
    ForgetViewer(st, player_id);
    st->watchers.erase(player_id);
    RemoveFromCell(st, gone);
    if (erase_entity) {
        st->entities.erase(player_id);
        st->entity_count.fetch_sub(1, std::memory_order_relaxed);
    } else {
        auto left = st->entities.find(player_id);
        if (left != st->entities.end())
            left->second.connected = false;
    }
    RefreshViewers(st, viewers, 0, 0, map_instance_id, pushes);
}

void MapRuntime::EmitTo(AoiPushBatch *pushes, uint64_t map_id, const MapEntity &recipient, int op,
                        const MapEntity &snapshot) const {
    if (!pushes || !recipient.connected || recipient.player_id == 0)
        return;
    pushes->map_instance_id = map_id;
    AoiRecipientEvent ev;
    ev.recipient_id = recipient.player_id;
    ev.gateway_instance_id = recipient.gateway_instance_id;
    ev.session_id = recipient.session_id;
    ev.op = op;
    ev.snapshot = snapshot;
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
            slot->view_radius = view_radius_cells >= 0 ? view_radius_cells : view_radius_;
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
    std::unique_lock<std::mutex> lock_a;
    std::unique_lock<std::mutex> lock_b;
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
        if (entity.x == 0 && entity.y == 0 && entity.z == 0) {
            entity.x = existing->second.x;
            entity.y = existing->second.y;
            entity.z = existing->second.z;
            entity.yaw = existing->second.yaw;
        }
        RemovePlayerInterest(st.get(), map_instance_id, entity.player_id, pushes, true);
    }
    entity.connected = true;
    if (entity.state_seq == 0)
        entity.state_seq = 1;
    st->entities[entity.player_id] = entity;
    st->entity_count.fetch_add(1, std::memory_order_relaxed);
    PlaceInCell(st.get(), entity);
    std::vector<uint64_t> viewers = NeighborhoodIds(*st, entity.x, entity.z);
    viewers.push_back(entity.player_id);
    RefreshViewers(st.get(), viewers, 0, entity.player_id, map_instance_id, pushes);
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
                               bool ignore_speed) {
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
    const auto hold0 = std::chrono::steady_clock::now();
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
    if (!st->data->InBounds(x, y, z))
        return fail(MapMoveReject::OutOfBounds, "ERR_OUT_OF_BOUNDS");
    float want_x = x, want_y = y, want_z = z;
    bool walked_slide = false;
    if (!st->data->IsWalkable(want_x, want_z)) {
        float sx = want_x, sz = want_z;
        if (!st->data->ClampOntoWalkable(e.x, e.z, want_x, want_z, &sx, &sz))
            return fail(MapMoveReject::Unwalkable, "ERR_UNWALKABLE");
        want_x = sx;
        want_z = sz;
        walked_slide = true;
    }
    const float dx = want_x - e.x, dy = want_y - e.y, dz = want_z - e.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int64_t t1 = now_ms > 0 ? now_ms : NowMs();
    float dt = 1.f;
    if (e.last_move_server_ms > 0 && t1 > e.last_move_server_ms)
        dt = static_cast<float>(t1 - e.last_move_server_ms) / 1000.f;
    const float speed = e.move_speed > 0.f ? e.move_speed : 10.f;
    const float max_dist = speed * dt * 1.25f + 0.5f;
    // 超速不整包拒绝：沿请求方向钳到本帧合法距离，避免客户端大跳拉回。
    if (!ignore_speed && dist > max_dist && dist > 1e-6f) {
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

    const float old_x = e.x;
    const float old_z = e.z;
    int old_cx = 0, old_cz = 0, new_cx = 0, new_cz = 0;
    st->data->WorldToAoiCell(old_x, old_z, &old_cx, &old_cz);
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
    PlaceInCell(st.get(), e);
    const bool same_cell = old_cx == new_cx && old_cz == new_cz;
    const auto neigh = NeighborhoodIds(*st, e.x, e.z);
    auto self_vis = st->visible_targets.find(player_id);
    bool membership_stable = same_cell && neigh.size() <= static_cast<size_t>(st->max_visible) + 1 &&
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
    if (membership_stable) {
        auto wit = st->watchers.find(player_id);
        if (wit != st->watchers.end()) {
            for (uint64_t vid : wit->second) {
                auto rit = st->entities.find(vid);
                if (rit == st->entities.end() || !rit->second.connected)
                    continue;
                EmitTo(pushes, map_instance_id, rit->second, 2, e);
                ServerStats::aoi_move_events.fetch_add(1, std::memory_order_relaxed);
                ServerStats::aoi_fast_move.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        std::vector<uint64_t> viewers = NeighborhoodIds(*st, old_x, old_z);
        viewers.insert(viewers.end(), neigh.begin(), neigh.end());
        auto wit = st->watchers.find(player_id);
        if (wit != st->watchers.end())
            viewers.insert(viewers.end(), wit->second.begin(), wit->second.end());
        viewers.push_back(player_id);
        RefreshViewers(st.get(), viewers, player_id, 0, map_instance_id, pushes);
    }
    if (confirmed)
        *confirmed = e;
    const auto hold_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - hold0)
                             .count();
    ServerStats::aoi_lock_hold_us_sum.fetch_add(static_cast<uint64_t>(hold_us < 0 ? 0 : hold_us),
                                                std::memory_order_relaxed);
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
        for (const auto &other : *aoi_snapshot)
            EmitTo(pushes, mid, e, 1, other);
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

std::vector<uint64_t> MapRuntime::ListPlayers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> out;
    out.reserve(player_map_.size());
    for (const auto &kv : player_map_)
        out.push_back(kv.first);
    return out;
}

void MapRuntime::ClearForTest() {
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
