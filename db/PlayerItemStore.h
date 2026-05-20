#pragma once

#include <cstdint>
#include <string>

class PlayerItemStore {
public:
    static PlayerItemStore &Instance();

    bool Available() const { return available_; }
    bool EnsureTable();
    bool Insert(uint64_t player_id, uint64_t item_id, uint32_t count, int64_t expire_time_sec,
                const std::string &extra_data, uint64_t *instance_id);

private:
    PlayerItemStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
