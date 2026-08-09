#include "MapInstanceRegistry.h"

#include <chrono>

MapInstanceRegistry &MapInstanceRegistry::Instance() {
    static MapInstanceRegistry g;
    return g;
}

int64_t MapInstanceRegistry::NowUnixSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

void MapInstanceRegistry::SetLocalInstanceId(std::string id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!id.empty())
        local_id_ = std::move(id);
}

bool MapInstanceRegistry::Claim(uint64_t map_instance_id, uint64_t map_template_id,
                               uint64_t owner_epoch, int64_t lease_until_unix) {
    if (map_instance_id == 0 || owner_epoch == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end()) {
        LocalMapInstance m;
        m.map_instance_id = map_instance_id;
        m.map_template_id = map_template_id;
        m.owner_epoch = owner_epoch;
        m.lease_until_unix = lease_until_unix;
        maps_[map_instance_id] = std::move(m);
        return true;
    }
    if (owner_epoch < it->second.owner_epoch)
        return false;
    if (owner_epoch > it->second.owner_epoch) {
        it->second.owner_epoch = owner_epoch;
        it->second.map_template_id = map_template_id;
        it->second.players.clear();
    }
    if (lease_until_unix > 0)
        it->second.lease_until_unix = lease_until_unix;
    return true;
}

void MapInstanceRegistry::SetLeaseUntil(uint64_t map_instance_id, int64_t lease_until_unix) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return;
    it->second.lease_until_unix = lease_until_unix;
}

int64_t MapInstanceRegistry::LeaseUntil(uint64_t map_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    return it == maps_.end() ? 0 : it->second.lease_until_unix;
}

MapWriteFence MapInstanceRegistry::CheckWrite(uint64_t map_instance_id,
                                              uint64_t owner_epoch) const {
    if (map_instance_id == 0 || owner_epoch == 0)
        return MapWriteFence::NotClaimed;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return MapWriteFence::NotClaimed;
    if (it->second.owner_epoch != owner_epoch)
        return MapWriteFence::StaleEpoch;
    if (it->second.lease_until_unix > 0 && NowUnixSec() >= it->second.lease_until_unix)
        return MapWriteFence::LeaseExpired;
    return MapWriteFence::Ok;
}

bool MapInstanceRegistry::AcceptWrite(uint64_t map_instance_id, uint64_t owner_epoch) const {
    return CheckWrite(map_instance_id, owner_epoch) == MapWriteFence::Ok;
}

bool MapInstanceRegistry::Has(uint64_t map_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return maps_.count(map_instance_id) > 0;
}

uint64_t MapInstanceRegistry::Epoch(uint64_t map_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    return it == maps_.end() ? 0 : it->second.owner_epoch;
}

bool MapInstanceRegistry::AddPlayer(uint64_t map_instance_id, uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return false;
    it->second.players.insert(player_id);
    return true;
}

bool MapInstanceRegistry::RemovePlayer(uint64_t map_instance_id, uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return false;
    it->second.players.erase(player_id);
    return true;
}

bool MapInstanceRegistry::PlayerOnMap(uint64_t map_instance_id, uint64_t player_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return false;
    return it->second.players.count(player_id) > 0;
}

uint32_t MapInstanceRegistry::PlayerCount(uint64_t map_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return 0;
    return static_cast<uint32_t>(it->second.players.size());
}

void MapInstanceRegistry::Release(uint64_t map_instance_id) {
    std::lock_guard<std::mutex> lk(mu_);
    maps_.erase(map_instance_id);
}

size_t MapInstanceRegistry::ReleaseExpired() {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = NowUnixSec();
    size_t n = 0;
    for (auto it = maps_.begin(); it != maps_.end();) {
        if (it->second.lease_until_unix > 0 && now >= it->second.lease_until_unix) {
            it = maps_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

std::vector<std::pair<uint64_t, uint64_t>> MapInstanceRegistry::ListOwned() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<uint64_t, uint64_t>> out;
    out.reserve(maps_.size());
    for (const auto &kv : maps_)
        out.emplace_back(kv.first, kv.second.owner_epoch);
    return out;
}

void MapInstanceRegistry::ClearForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    maps_.clear();
}
