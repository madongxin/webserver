#pragma once

#include "ServerStats.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

struct AoiMoveStamp {
    uint64_t entity_id = 0;
    uint64_t state_seq = 0;
};

/**
 * 40 个移动事件打成一帧后的平均字节，含 4 字节长度头和 GameResponse。
 * 实测 frame_bytes=1498，1498/40=37.45，向上取整 38。单事件帧是 54 字节。
 * 见 docs/perf_audit/aoi_move_egress.md。
 */
constexpr int kAoiMoveEventBytes = 38;

/**
 * 一条连接上的 MOVE 合并账本。槽是唯一事实来源，发出去的帧只是一份可丢弃的快照。
 * legacy_restore=true 复现修复前的行为：写出不回写账本，被替换的人下一轮仍排最前。
 */
class AoiMoveCoalesceLedger {
public:
    struct Entity {
        uint64_t state_seq = 0;
        int64_t pending_since_ms = 0;
        int64_t last_written_ms = 0;
        int unwritten_attempts = 0;
        bool in_flight = false;
        uint64_t in_flight_seq = 0;
        int layer = -1;  // 1 near, 0 far, -1 unknown
        float x = 0;
        float z = 0;
        float sent_x = 0;
        float sent_z = 0;
        bool has_pos = false;
        bool has_sent_pos = false;
    };

    void EnableLegacyAccountingForTest(bool on) { legacy_ = on; }

    void Offer(uint64_t id, uint64_t seq, int64_t now, int layer, float x = 0, float z = 0,
               bool has_pos = false) {
        if (id == 0)
            return;
        auto &ent = entities_[id];
        if (ent.state_seq != 0 && seq < ent.state_seq)
            return;
        const bool waiting = ent.pending_since_ms > 0;
        ent.state_seq = seq;
        ent.layer = layer;
        if (has_pos) {
            ent.x = x;
            ent.z = z;
            ent.has_pos = true;
        }
        if (!waiting)
            ent.pending_since_ms = now;
    }

    std::vector<AoiMoveStamp> Pick(int64_t now, int per_conn_budget, int est_bytes, int max_n,
                                   int64_t *wait_ms) {
        if (wait_ms)
            *wait_ms = 0;
        if (max_n < 1)
            max_n = 1;
        if (est_bytes < 1)
            est_bytes = 1;
        const int64_t oldest = MaxSilenceMs(now);
        const bool force = oldest >= 1000;
        if (!force && last_written_flush_ms_ != 0 && now - last_written_flush_ms_ < 200) {
            if (wait_ms)
                *wait_ms = 200 - (now - last_written_flush_ms_);
            return {};
        }
        if (budget_window_ms_ == 0 || now - budget_window_ms_ >= 1000) {
            budget_window_ms_ = now;
            budget_bytes_ = 0;
        }
        int budget = per_conn_budget > 0 ? per_conn_budget : 16 * 1024;
        int remain = budget - budget_bytes_;
        int limit = 0;
        if (force) {
            // 强制带上最老且最近没被尝试的一批，仍不超过每帧名额。
            // 全局预算已经用完时只带 1 个，避免把同一批字节再推进重传。
            limit = remain >= est_bytes ? max_n : 1;
        } else if (remain >= est_bytes) {
            limit = remain / est_bytes;
            if (limit > max_n)
                limit = max_n;
        }
        if (limit <= 0) {
            if (wait_ms) {
                *wait_ms = 1000 - (now - budget_window_ms_);
                if (*wait_ms < 50)
                    *wait_ms = 50;
            }
            ServerStats::aoi_move_budget_defer.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        struct Cand {
            uint64_t id = 0;
            int64_t gap = 0;
            int attempts = 0;
            int layer = -1;
            bool hard = false;
        };
        std::vector<Cand> cands;
        cands.reserve(entities_.size());
        for (const auto &kv : entities_) {
            if (kv.second.in_flight || kv.second.pending_since_ms <= 0)
                continue;
            Cand c;
            c.id = kv.first;
            c.gap = kv.second.last_written_ms <= 0 ? now + 1 : now - kv.second.last_written_ms;
            c.attempts = kv.second.unwritten_attempts;
            if (legacy_) {
                c.gap = now - kv.second.pending_since_ms;
                c.attempts = 0;
            }
            cands.push_back(c);
        }
        if (cands.empty())
            return {};
        // 预算不够时，满 1 秒也只放行 1 个最老的，避免把整帧再推进重传。
        // 静默到 15 秒的人排在前面，但仍受本帧名额和预算限制，超出的名额不往缓冲里堆。
        if (force && remain < est_bytes)
            limit = std::max(limit, 1);
        const int take_n = std::min(limit, static_cast<int>(cands.size()));
        std::partial_sort(cands.begin(), cands.begin() + take_n, cands.end(),
                          [](const Cand &a, const Cand &b) {
                              if (a.gap != b.gap)
                                  return a.gap > b.gap;
                              if (a.attempts != b.attempts)
                                  return a.attempts < b.attempts;
                              return a.id < b.id;
                          });
        const std::vector<Cand> &chosen = cands;
        std::vector<AoiMoveStamp> out;
        out.reserve(static_cast<size_t>(take_n));
        int repicked = 0;
        for (int i = 0; i < take_n; ++i) {
            const uint64_t id = chosen[static_cast<size_t>(i)].id;
            auto &ent = entities_[id];
            if (ent.unwritten_attempts > 0)
                ++repicked;
            AoiMoveStamp stamp;
            stamp.entity_id = id;
            stamp.state_seq = ent.state_seq;
            out.push_back(stamp);
        }
        ServerStats::aoi_move_repicked_unwritten.store(static_cast<uint64_t>(repicked),
                                                       std::memory_order_relaxed);
        ServerStats::aoi_move_repicked_samples.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<int>(entities_.size()) > take_n)
            ServerStats::aoi_move_capped.fetch_add(
                static_cast<uint64_t>(entities_.size() - static_cast<size_t>(take_n)),
                std::memory_order_relaxed);
        return out;
    }

    void NoteParked(const std::vector<AoiMoveStamp> &frame) {
        for (const auto &stamp : frame) {
            auto it = entities_.find(stamp.entity_id);
            if (it == entities_.end())
                continue;
            it->second.in_flight = !legacy_;
            it->second.in_flight_seq = stamp.state_seq;
        }
    }

    void NoteReplaced(const std::vector<AoiMoveStamp> &frame) {
        ServerStats::aoi_move_park_replaced.fetch_add(1, std::memory_order_relaxed);
        for (const auto &stamp : frame) {
            auto it = entities_.find(stamp.entity_id);
            if (it == entities_.end())
                continue;
            it->second.in_flight = false;
            if (!legacy_)
                ++it->second.unwritten_attempts;
            // 等待时间保持原值，不能因为换了一帧快照就重新计时。
        }
    }

    void NoteKernelWrite(const std::vector<AoiMoveStamp> &frame, int64_t now, int frame_bytes) {
        ServerStats::aoi_move_commit.fetch_add(1, std::memory_order_relaxed);
        ServerStats::aoi_move_park_written.fetch_add(1, std::memory_order_relaxed);
        if (legacy_)
            return;
        if (frame_bytes > 0) {
            budget_bytes_ += frame_bytes;
            ServerStats::egress_admitted_bytes.fetch_add(static_cast<uint64_t>(frame_bytes),
                                                         std::memory_order_relaxed);
        }
        last_written_flush_ms_ = now;
        for (const auto &stamp : frame) {
            auto it = entities_.find(stamp.entity_id);
            if (it == entities_.end())
                continue;
            it->second.in_flight = false;
            it->second.last_written_ms = now;
            it->second.unwritten_attempts = 0;
            it->second.pending_since_ms = 0;
            if (it->second.has_pos) {
                it->second.sent_x = it->second.x;
                it->second.sent_z = it->second.z;
                it->second.has_sent_pos = true;
            }
            if (it->second.state_seq != stamp.state_seq)
                it->second.pending_since_ms = now;
        }
    }

    int64_t MaxSilenceMs(int64_t now) const {
        int64_t oldest = 0;
        for (const auto &kv : entities_) {
            if (kv.second.pending_since_ms <= 0 || now < kv.second.pending_since_ms)
                continue;
            const int64_t age = now - kv.second.pending_since_ms;
            if (age > oldest)
                oldest = age;
        }
        return oldest;
    }

    void AppendSilence(int64_t now, std::vector<int64_t> *near, std::vector<int64_t> *far) const {
        for (const auto &kv : entities_) {
            if (kv.second.pending_since_ms <= 0 || now < kv.second.pending_since_ms)
                continue;
            const int64_t age = now - kv.second.pending_since_ms;
            if (kv.second.layer == 1) {
                if (near)
                    near->push_back(age);
            } else if (kv.second.layer == 0 && far) {
                far->push_back(age);
            }
        }
    }

    size_t waiting() const { return entities_.size(); }

    bool IsInFlight(uint64_t id) const {
        auto it = entities_.find(id);
        return it != entities_.end() && it->second.in_flight;
    }

    static int EgressBudgetBytesPerSec() {
        static int bytes = [] {
            const char *env = std::getenv("GAMEMESH_EGRESS_BUDGET_MBPS");
            double mbps = (env && *env) ? std::atof(env) : 2.4;
            if (mbps < 0.1)
                mbps = 0.1;
            const int v = static_cast<int>(mbps * 1000000.0 / 8.0);
            ServerStats::egress_budget_bytes_per_sec.store(v, std::memory_order_relaxed);
            return v;
        }();
        return bytes;
    }

private:
    bool legacy_ = false;
    std::unordered_map<uint64_t, Entity> entities_;
    int64_t last_written_flush_ms_ = 0;
    int64_t budget_window_ms_ = 0;
    int budget_bytes_ = 0;
};
