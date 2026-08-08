#include "PushReplayCache.h"

PushReplayCache &PushReplayCache::Instance() {
    static PushReplayCache g;
    return g;
}

void PushReplayCache::Configure(size_t per_player_cap) {
    std::lock_guard<std::mutex> lk(mu_);
    if (per_player_cap > 0)
        cap_ = per_player_cap;
}

uint64_t PushReplayCache::NextSeq(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    return ++next_seq_[player_id];
}

void PushReplayCache::Store(uint64_t player_id, const PushReplayEntry &entry) {
    if (player_id == 0 || !entry.reliable)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    auto &q = by_player_[player_id];
    q.push_back(entry);
    while (q.size() > cap_)
        q.pop_front();
    if (entry.server_seq >= next_seq_[player_id])
        next_seq_[player_id] = entry.server_seq;
}

bool PushReplayCache::ReplayAfter(uint64_t player_id, uint64_t last_acked_seq,
                                  std::vector<PushReplayEntry> *out, bool *need_snapshot) {
    if (!out)
        return false;
    out->clear();
    if (need_snapshot)
        *need_snapshot = false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_player_.find(player_id);
    if (it == by_player_.end() || it->second.empty()) {
        if (last_acked_seq == 0)
            return true;
        if (need_snapshot)
            *need_snapshot = true;
        return false;
    }
    const auto &q = it->second;
    if (last_acked_seq + 1 < q.front().server_seq) {
        if (need_snapshot)
            *need_snapshot = true;
        return false;
    }
    for (const auto &e : q) {
        if (e.server_seq > last_acked_seq)
            out->push_back(e);
    }
    return true;
}

void PushReplayCache::ClearPlayer(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    by_player_.erase(player_id);
    next_seq_.erase(player_id);
}
