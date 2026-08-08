#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PushReplayEntry {
    uint64_t server_seq = 0;
    std::string message_type;
    std::string payload;
    bool reliable = true;
};

/**
 * 每玩家有界可靠 Push 回放缓存。
 * 重连时按 last_acked_seq 回放；缺口过大返回 need_snapshot。
 */
class PushReplayCache {
public:
    static PushReplayCache &Instance();

    void Configure(size_t per_player_cap = 64);

    uint64_t NextSeq(uint64_t player_id);
    void Store(uint64_t player_id, const PushReplayEntry &entry);

    /** @return false 且 *need_snapshot=true 表示缓存不足需全量同步 */
    bool ReplayAfter(uint64_t player_id, uint64_t last_acked_seq,
                     std::vector<PushReplayEntry> *out, bool *need_snapshot);

    void ClearPlayer(uint64_t player_id);

private:
    PushReplayCache() = default;
    std::mutex mu_;
    size_t cap_ = 64;
    std::unordered_map<uint64_t, uint64_t> next_seq_;
    std::unordered_map<uint64_t, std::deque<PushReplayEntry>> by_player_;
};
