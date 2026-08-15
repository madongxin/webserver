#include "MapRuntime.h"

#include <chrono>
#include <cmath>

namespace {

bool Finite4(float a, float b, float c, float d) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
}

}  // namespace

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
    std::unordered_set<uint64_t> out;
    if (!st.data)
        return out;
    int cx = 0, cz = 0;
    st.data->WorldToAoiCell(e.x, e.z, &cx, &cz);
    const int r = view_radius_;
    for (int dx = -r; dx <= r; ++dx) {
        for (int dz = -r; dz <= r; ++dz) {
            auto it = st.cells.find(CellKey(cx + dx, cz + dz));
            if (it == st.cells.end())
                continue;
            for (uint64_t pid : it->second) {
                if (pid == e.player_id)
                    continue;
                auto eit = st.entities.find(pid);
                if (eit != st.entities.end() && eit->second.connected)
                    out.insert(pid);
            }
        }
    }
    return out;
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
                       AoiPushBatch *pushes, std::string *err) {
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
    } else if (!slot->data) {
        slot->data = std::move(data);
    }
    InstanceState &st = *slot;
    const auto old_map = player_map_.find(entity.player_id);
    if (old_map != player_map_.end() && old_map->second != map_instance_id) {
        // 跨实例：先从旧图摘除（锁内，无 IO）
        auto oit = maps_.find(old_map->second);
        if (oit != maps_.end() && oit->second) {
            auto eit = oit->second->entities.find(entity.player_id);
            if (eit != oit->second->entities.end()) {
                const MapEntity gone = eit->second;
                const auto vis = VisibleIds(*oit->second, gone);
                RemoveFromCell(oit->second.get(), gone);
                oit->second->entities.erase(eit);
                for (uint64_t vid : vis) {
                    auto rit = oit->second->entities.find(vid);
                    if (rit != oit->second->entities.end())
                        EmitTo(pushes, old_map->second, rit->second, 3, gone);
                }
            }
        }
    }
    auto existing = st.entities.find(entity.player_id);
    if (existing != st.entities.end()) {
        RemoveFromCell(&st, existing->second);
        entity.state_seq = existing->second.state_seq;
        entity.last_client_seq = existing->second.last_client_seq;
        entity.last_move_server_ms = existing->second.last_move_server_ms;
        if (entity.x == 0 && entity.y == 0 && entity.z == 0) {
            entity.x = existing->second.x;
            entity.y = existing->second.y;
            entity.z = existing->second.z;
            entity.yaw = existing->second.yaw;
        }
    }
    entity.connected = true;
    if (entity.state_seq == 0)
        entity.state_seq = 1;
    st.entities[entity.player_id] = entity;
    PlaceInCell(&st, entity);
    player_map_[entity.player_id] = map_instance_id;
    const auto vis = VisibleIds(st, entity);
    if (aoi_snapshot)
        aoi_snapshot->clear();
    for (uint64_t vid : vis) {
        auto it = st.entities.find(vid);
        if (it == st.entities.end())
            continue;
        if (aoi_snapshot)
            aoi_snapshot->push_back(it->second);
        EmitTo(pushes, map_instance_id, it->second, 1, entity);
    }
    if (self_out)
        *self_out = entity;
    return true;
}

MapMoveReject MapRuntime::Move(uint64_t map_instance_id, uint64_t player_id, float x, float y,
                               float z, float yaw, uint64_t client_seq, int64_t now_ms,
                               MapEntity *confirmed, AoiPushBatch *pushes, std::string *err_code) {
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
    if (client_seq != 0 && e.last_client_seq != 0 && client_seq <= e.last_client_seq)
        return fail(MapMoveReject::StaleSeq, "ERR_STALE_SEQ");
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
    if (dist > max_dist)
        return fail(MapMoveReject::TooFast, "ERR_MOVE_TOO_FAST");

    const auto old_vis = VisibleIds(st, e);
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
    const auto new_vis = VisibleIds(st, e);

    for (uint64_t vid : old_vis) {
        if (new_vis.count(vid))
            continue;
        auto rit = st.entities.find(vid);
        if (rit != st.entities.end())
            EmitTo(pushes, map_instance_id, rit->second, 3, e);
    }
    for (uint64_t vid : new_vis) {
        auto rit = st.entities.find(vid);
        if (rit == st.entities.end())
            continue;
        if (!old_vis.count(vid))
            EmitTo(pushes, map_instance_id, rit->second, 1, e);
        else
            EmitTo(pushes, map_instance_id, rit->second, 2, e);
    }
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
    const MapEntity gone = eit->second;
    const auto vis = VisibleIds(st, gone);
    RemoveFromCell(&st, gone);
    st.entities.erase(eit);
    player_map_.erase(player_id);
    for (uint64_t vid : vis) {
        auto rit = st.entities.find(vid);
        if (rit != st.entities.end())
            EmitTo(pushes, map_instance_id, rit->second, 3, gone);
    }
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
    const MapEntity gone = eit->second;
    const auto vis = VisibleIds(st, gone);
    RemoveFromCell(&st, gone);
    st.entities.erase(eit);
    player_map_.erase(player_id);
    for (uint64_t vid : vis) {
        auto rit = st.entities.find(vid);
        if (rit != st.entities.end())
            EmitTo(pushes, mid, rit->second, 3, gone);
    }
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
    const auto vis = VisibleIds(st, e);
    RemoveFromCell(&st, e);
    e.connected = false;
    for (uint64_t vid : vis) {
        auto rit = st.entities.find(vid);
        if (rit != st.entities.end())
            EmitTo(pushes, mid, rit->second, 3, e);
    }
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
    const auto vis = VisibleIds(st, e);
    if (aoi_snapshot)
        aoi_snapshot->clear();
    for (uint64_t vid : vis) {
        auto it = st.entities.find(vid);
        if (it == st.entities.end())
            continue;
        if (aoi_snapshot)
            aoi_snapshot->push_back(it->second);
        EmitTo(pushes, mid, it->second, 1, e);
    }
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

void MapRuntime::ClearForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    maps_.clear();
    player_map_.clear();
}
