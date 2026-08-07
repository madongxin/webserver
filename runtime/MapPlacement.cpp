#include "MapPlacement.h"

MapPlacement &MapPlacement::Instance() {
    static MapPlacement g;
    return g;
}

void MapPlacement::ConfigureOwners(std::vector<std::string> owner_ids) {
    std::lock_guard<std::mutex> lk(mu_);
    owners_ = std::move(owner_ids);
    rr_ = 0;
}

bool MapPlacement::ResolveOrAllocate(uint32_t realm_id, uint64_t map_template_id,
                                     uint64_t map_instance_id, MapPlacementRecord *out) {
    if (!out)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (map_instance_id != 0) {
        auto it = table_.find(map_instance_id);
        if (it == table_.end())
            return false;
        *out = it->second;
        return true;
    }
    if (owners_.empty() || map_template_id == 0)
        return false;
    MapPlacementRecord rec;
    rec.realm_id = realm_id;
    rec.map_template_id = map_template_id;
    rec.map_instance_id = next_instance_id_++;
    rec.owner_gamelogic_id = owners_[rr_ % owners_.size()];
    ++rr_;
    rec.owner_epoch = 1;
    rec.route_version = 1;
    rec.frozen = false;
    table_[rec.map_instance_id] = rec;
    *out = rec;
    return true;
}

bool MapPlacement::Get(uint64_t map_instance_id, MapPlacementRecord *out) const {
    if (!out || map_instance_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(map_instance_id);
    if (it == table_.end())
        return false;
    *out = it->second;
    return true;
}

bool MapPlacement::Migrate(uint64_t map_instance_id, const std::string &new_owner,
                           MapPlacementRecord *out) {
    if (new_owner.empty())
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(map_instance_id);
    if (it == table_.end())
        return false;
    it->second.owner_gamelogic_id = new_owner;
    it->second.owner_epoch += 1;
    it->second.route_version += 1;
    it->second.frozen = false;
    if (out)
        *out = it->second;
    return true;
}

void MapPlacement::ClearForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    table_.clear();
    next_instance_id_ = 1;
    rr_ = 0;
}
