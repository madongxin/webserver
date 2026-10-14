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
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = player_map_.find(player_id);
    if (mit == player_map_.end())
        return;
    auto iit = maps_.find(mit->second);
    if (iit == maps_.end() || !iit->second)
        return;
    auto eit = iit->second->entities.find(player_id);
    if (eit != iit->second->entities.end())
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
    // 视野格子里只同步最近的 50 人。距离相同按 player_id，避免叠点时名单来回抖动。
    constexpr size_t kAoiSyncCap = 50;
    struct Cand {
        uint64_t id = 0;
        float d2 = 0.f;
    };
    std::vector<Cand> cands;
    if (!st.data)
        return {};
    int cx = 0, cz = 0;
    st.data->WorldToAoiCell(e.x, e.z, &cx, &cz);
    const int r = st.view_radius >= 0 ? st.view_radius : view_radius_;
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
    if (cands.size() > kAoiSyncCap) {
        std::nth_element(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(kAoiSyncCap),
                         cands.end(), [](const Cand &a, const Cand &b) {
                             if (a.d2 < b.d2)
                                 return true;
                             if (b.d2 < a.d2)
                                 return false;
                             return a.id < b.id;
                         });
        cands.resize(kAoiSyncCap);
    }
    std::unordered_set<uint64_t> out;
    out.reserve(cands.size());
    for (const auto &c : cands)
        out.insert(c.id);
    return out;
}

std::vector<uint64_t> MapRuntime::NeighborhoodIds(const InstanceState &st, float x, float z) const {
    std::vector<uint64_t> ids;
    if (!st.data || st.view_radius <= 0)
        return ids;
    const float cell = st.data->aoi_cell_size() > 0.f ? st.data->aoi_cell_size() : 32.f;
    const int cx = static_cast<int>(std::floor(x / cell));
    const int cz = static_cast<int>(std::floor(z / cell));
    for (int dz = -st.view_radius; dz <= st.view_radius; ++dz) {
        for (int dx = -st.view_radius; dx <= st.view_radius; ++dx) {
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
            if (tit != st->entities.end())
                EmitTo(pushes, map_instance_id, viewer, 1, tit->second);
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
        }
        if (neu.empty())
            st->visible_targets.erase(viewer_id);
        else
            st->visible_targets[viewer_id] = neu;
        if (mover_id != 0 && mover_id != viewer_id && old.count(mover_id) && neu.count(mover_id)) {
            auto mit = st->entities.find(mover_id);
            if (mit != st->entities.end())
                EmitTo(pushes, map_instance_id, viewer, 2, mit->second);
        }
    }
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
                       AoiPushBatch *pushes, std::string *err, int view_radius_cells) {
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
    std::lock_guard<std::mutex> lk(mu_);
    auto &slot = maps_[map_instance_id];
    if (!slot) {
        slot.reset(new InstanceState());
        slot->data = std::move(data);
        slot->view_radius = view_radius_cells >= 0 ? view_radius_cells : view_radius_;
    } else if (!slot->data) {
        slot->data = std::move(data);
        if (slot->view_radius < 0)
            slot->view_radius = view_radius_cells >= 0 ? view_radius_cells : view_radius_;
    }
    InstanceState &st = *slot;
    const auto old_map = player_map_.find(entity.player_id);
    if (old_map != player_map_.end() && old_map->second != map_instance_id) {
        auto oit = maps_.find(old_map->second);
        if (oit != maps_.end() && oit->second) {
            RemovePlayerInterest(oit->second.get(), old_map->second, entity.player_id, pushes, true);
            if (oit->second->entities.empty())
                maps_.erase(oit);
        }
        player_map_.erase(entity.player_id);
    }
    auto existing = st.entities.find(entity.player_id);
    if (existing != st.entities.end()) {
        entity.state_seq = existing->second.state_seq;
        entity.last_client_seq = 0;
        entity.last_move_server_ms = existing->second.last_move_server_ms;
        if (entity.x == 0 && entity.y == 0 && entity.z == 0) {
            entity.x = existing->second.x;
            entity.y = existing->second.y;
            entity.z = existing->second.z;
            entity.yaw = existing->second.yaw;
        }
        RemovePlayerInterest(&st, map_instance_id, entity.player_id, pushes, true);
    }
    entity.connected = true;
    if (entity.state_seq == 0)
        entity.state_seq = 1;
    st.entities[entity.player_id] = entity;
    PlaceInCell(&st, entity);
    player_map_[entity.player_id] = map_instance_id;
    std::vector<uint64_t> viewers = NeighborhoodIds(st, entity.x, entity.z);
    viewers.push_back(entity.player_id);
    RefreshViewers(&st, viewers, 0, entity.player_id, map_instance_id, pushes);
    FillSnapshot(st, entity.player_id, aoi_snapshot);
    if (self_out)
        *self_out = entity;
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
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = maps_.find(map_instance_id);
    if (mit == maps_.end() || !mit->second)
        return fail(MapMoveReject::NotOnMap, "ERR_NOT_ON_MAP");
    InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end())
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
    if (!st.data)
        return fail(MapMoveReject::NotOnMap, "ERR_MAP_RUNTIME_NOT_READY");
    if (!st.data->InBounds(x, y, z))
        return fail(MapMoveReject::OutOfBounds, "ERR_OUT_OF_BOUNDS");
    if (!st.data->IsWalkable(x, z))
        return fail(MapMoveReject::Unwalkable, "ERR_UNWALKABLE");
    const float dx = x - e.x, dy = y - e.y, dz = z - e.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int64_t t1 = now_ms > 0 ? now_ms : NowMs();
    float dt = 1.f;
    if (e.last_move_server_ms > 0 && t1 > e.last_move_server_ms)
        dt = static_cast<float>(t1 - e.last_move_server_ms) / 1000.f;
    const float speed = e.move_speed > 0.f ? e.move_speed : 10.f;
    const float max_dist = speed * dt * 1.25f + 0.5f;
    if (!ignore_speed && dist > max_dist)
        return fail(MapMoveReject::TooFast, "ERR_MOVE_TOO_FAST");

    const float old_x = e.x;
    const float old_z = e.z;
    RemoveFromCell(&st, e);
    e.x = x;
    e.y = y;
    e.z = z;
    e.yaw = yaw;
    e.state_seq += 1;
    if (client_seq != 0)
        e.last_client_seq = client_seq;
    e.last_move_server_ms = t1;
    PlaceInCell(&st, e);
    std::vector<uint64_t> viewers = NeighborhoodIds(st, old_x, old_z);
    const auto neu_n = NeighborhoodIds(st, e.x, e.z);
    viewers.insert(viewers.end(), neu_n.begin(), neu_n.end());
    auto wit = st.watchers.find(player_id);
    if (wit != st.watchers.end())
        viewers.insert(viewers.end(), wit->second.begin(), wit->second.end());
    viewers.push_back(player_id);
    RefreshViewers(&st, viewers, player_id, 0, map_instance_id, pushes);
    if (confirmed)
        *confirmed = e;
    return MapMoveReject::Ok;
}

bool MapRuntime::Leave(uint64_t map_instance_id, uint64_t player_id, AoiPushBatch *pushes) {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = maps_.find(map_instance_id);
    if (mit == maps_.end() || !mit->second)
        return false;
    InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end())
        return false;
    RemovePlayerInterest(&st, map_instance_id, player_id, pushes, true);
    player_map_.erase(player_id);
    if (st.entities.empty())
        maps_.erase(mit);
    return true;
}

void MapRuntime::LeaveAll(uint64_t player_id, AoiPushBatch *pushes) {
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = player_map_.find(player_id);
    if (pit == player_map_.end())
        return;
    const uint64_t mid = pit->second;
    auto mit = maps_.find(mid);
    if (mit == maps_.end() || !mit->second)
        return;
    InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end()) {
        player_map_.erase(pit);
        return;
    }
    RemovePlayerInterest(&st, mid, player_id, pushes, true);
    player_map_.erase(player_id);
    if (st.entities.empty())
        maps_.erase(mit);
}

bool MapRuntime::Disconnect(uint64_t player_id, AoiPushBatch *pushes) {
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = player_map_.find(player_id);
    if (pit == player_map_.end())
        return false;
    const uint64_t mid = pit->second;
    auto mit = maps_.find(mid);
    if (mit == maps_.end() || !mit->second)
        return false;
    InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end())
        return false;
    MapEntity &e = eit->second;
    if (!e.connected)
        return true;
    RemovePlayerInterest(&st, mid, player_id, pushes, false);
    return true;
}

bool MapRuntime::Reconnect(uint64_t player_id, const std::string &gateway_instance_id,
                           const std::string &session_id, MapEntity *self_out,
                           std::vector<MapEntity> *aoi_snapshot, AoiPushBatch *pushes) {
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = player_map_.find(player_id);
    if (pit == player_map_.end())
        return false;
    const uint64_t mid = pit->second;
    auto mit = maps_.find(mid);
    if (mit == maps_.end() || !mit->second)
        return false;
    InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end())
        return false;
    MapEntity &e = eit->second;
    e.connected = true;
    if (!gateway_instance_id.empty())
        e.gateway_instance_id = gateway_instance_id;
    if (!session_id.empty())
        e.session_id = session_id;
    PlaceInCell(&st, e);
    std::vector<uint64_t> viewers = NeighborhoodIds(st, e.x, e.z);
    viewers.push_back(player_id);
    RefreshViewers(&st, viewers, 0, player_id, mid, pushes);
    FillSnapshot(st, player_id, aoi_snapshot);
    if (self_out)
        *self_out = e;
    return true;
}

bool MapRuntime::HasPlayer(uint64_t map_instance_id, uint64_t player_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = maps_.find(map_instance_id);
    if (mit == maps_.end() || !mit->second)
        return false;
    return mit->second->entities.count(player_id) != 0;
}

uint32_t MapRuntime::ConnectedCount(uint64_t map_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = maps_.find(map_instance_id);
    if (mit == maps_.end() || !mit->second)
        return 0;
    uint32_t n = 0;
    for (const auto &kv : mit->second->entities) {
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
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = player_map_.find(player_id);
    if (pit == player_map_.end())
        return false;
    const uint64_t mid = pit->second;
    auto mit = maps_.find(mid);
    if (mit == maps_.end() || !mit->second)
        return false;
    const InstanceState &st = *mit->second;
    auto eit = st.entities.find(player_id);
    if (eit == st.entities.end())
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
    FillSnapshot(st, player_id, aoi_snapshot);
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
    std::lock_guard<std::mutex> lk(mu_);
    maps_.clear();
    player_map_.clear();
}

bool MapRuntime::Unload(uint64_t map_instance_id) {
    if (map_instance_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end() || !it->second)
        return false;
    for (const auto &kv : it->second->entities)
        player_map_.erase(kv.first);
    maps_.erase(it);
    return true;
}
