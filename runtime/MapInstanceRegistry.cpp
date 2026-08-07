#include "MapInstanceRegistry.h"

MapInstanceRegistry &MapInstanceRegistry::Instance() {
    static MapInstanceRegistry g;
    return g;
}

void MapInstanceRegistry::SetLocalInstanceId(std::string id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!id.empty())
        local_id_ = std::move(id);
}

bool MapInstanceRegistry::Claim(uint64_t map_instance_id, uint64_t map_template_id,
                               uint64_t owner_epoch) {
    if (map_instance_id == 0 || owner_epoch == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end()) {
        LocalMapInstance m;
        m.map_instance_id = map_instance_id;
        m.map_template_id = map_template_id;
        m.owner_epoch = owner_epoch;
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
    return true;
}

bool MapInstanceRegistry::AcceptWrite(uint64_t map_instance_id, uint64_t owner_epoch) const {
    if (map_instance_id == 0 || owner_epoch == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(map_instance_id);
    if (it == maps_.end())
        return false;
    return it->second.owner_epoch == owner_epoch;
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

void MapInstanceRegistry::ClearForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    maps_.clear();
}
