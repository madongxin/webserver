#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct FriendPresenceDest {
    uint64_t player_id = 0;
    std::string gateway_id;
};

struct FriendPresenceBatch {
    std::string gateway_id;
    std::vector<uint64_t> player_ids;
};

/** 同一 gateway 的在线好友合成若干批，单批不超过 batch_max。 */
inline std::vector<FriendPresenceBatch>
SplitPresenceBatches(const std::vector<FriendPresenceDest> &dests, size_t batch_max) {
    if (batch_max == 0)
        batch_max = 200;
    std::map<std::string, std::vector<uint64_t>> grouped;
    for (const auto &dest : dests) {
        if (dest.player_id == 0 || dest.gateway_id.empty())
            continue;
        grouped[dest.gateway_id].push_back(dest.player_id);
    }
    std::vector<FriendPresenceBatch> out;
    for (auto &item : grouped) {
        FriendPresenceBatch batch;
        batch.gateway_id = item.first;
        for (uint64_t id : item.second) {
            if (batch.player_ids.size() >= batch_max) {
                out.push_back(std::move(batch));
                batch = FriendPresenceBatch{};
                batch.gateway_id = item.first;
            }
            batch.player_ids.push_back(id);
        }
        if (!batch.player_ids.empty())
            out.push_back(std::move(batch));
    }
    return out;
}
