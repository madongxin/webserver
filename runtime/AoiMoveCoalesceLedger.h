#pragma once

#include "MoveSendConfig.h"
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
 * 位置包现在带上名字和血量，避免客户端把缺省的 0 画成 0/0 后停住。
 * 原来 40 人一帧约 38 字节/人。名字和 hp、max_hp 大约再加 14 字节。
 */
constexpr int kAoiMoveEventBytes = 52;
/** 带 vx、vz、move_start_ms 的粗算字节。待压测校准。预算不提高，估大只会少发。 */
constexpr int kAoiMoveExtrapEventBytes = 58;
/**
 * 一个 TCP 小段的线上开销：以太网头 14 + IPv4 头 20 + TCP 头 20 + 12 字节选项/填充余量。
 * 预算按 payload + 这个常量记账，避免 54 字节小帧把真实流量算少。
 */
constexpr int kOnWireHeaderBytes = 66;
constexpr int kNearQuotaPercent = 60;
constexpr int kBatchMinEvents = 8;
constexpr int kNearHardSilenceMs = 3000;  // 待压测校准
constexpr int kFarHardSilenceMs = 15000;  // 待压测校准
constexpr int kBudgetReservePercent = 10;

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
    void SetSendIntervalMs(int ms) { coalesce_interval_ms_ = ms; }
    void SetQuotaPercent(int pct) { quota_numerator_ = pct; }
    int charged_total() const { return charged_total_; }
    void ClearPending() {
        for (auto &kv : entities_) {
            kv.second.pending_since_ms = 0;
            kv.second.in_flight = false;
        }
    }

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
        const int interval = coalesce_interval_ms_ > 0 ? coalesce_interval_ms_ : 200;
        if (last_written_flush_ms_ != 0 && now - last_written_flush_ms_ < interval) {
            if (wait_ms)
                *wait_ms = interval - (now - last_written_flush_ms_);
            return {};
        }
            if (budget_window_ms_ == 0 || now - budget_window_ms_ >= 1000) {
            budget_window_ms_ = now;
            budget_bytes_ = 0;
            over_reported_ = 0;
        }
        window_budget_ = per_conn_budget > 0 ? per_conn_budget : 16 * 1024;
        int budget = per_conn_budget > 0 ? per_conn_budget : 16 * 1024;
        if (quota_numerator_ > 0 && quota_numerator_ < 100)
            max_n = std::max(1, max_n * quota_numerator_ / 100);
        const int normal_budget = budget * (100 - kBudgetReservePercent) / 100;
        bool any_hard = false;
        if (!legacy_) {
            for (const auto &kv : entities_) {
                if (kv.second.in_flight || kv.second.pending_since_ms <= 0)
                    continue;
                const int64_t age = now - kv.second.pending_since_ms;
                const int cap = kv.second.layer == 1 ? NearHardSilenceMs() : FarHardSilenceMs();
                if (age >= cap)
                    any_hard = true;
            }
        }
        const int ceiling = any_hard ? budget : normal_budget;
        int remain = ceiling - budget_bytes_;
        if (remain < 0)
            remain = 0;
        int limit = 0;
        if (legacy_) {
            const bool force = oldest >= 1000;
            if (force)
                limit = remain >= est_bytes ? max_n : 1;
            else if (remain >= est_bytes)
                limit = std::min(max_n, remain / est_bytes);
            if (force && remain < est_bytes)
                limit = std::max(limit, 1);
        } else if (remain >= est_bytes + kOnWireHeaderBytes) {
            limit = (remain - kOnWireHeaderBytes) / est_bytes;
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
        auto better = [](const Cand &a, const Cand &b) {
            if (a.attempts != b.attempts)
                return a.attempts < b.attempts;
            if (a.gap != b.gap)
                return a.gap > b.gap;
            return a.id < b.id;
        };
        std::vector<Cand> chosen;
        if (legacy_) {
            const int take_n = std::min(limit, static_cast<int>(cands.size()));
            std::partial_sort(cands.begin(), cands.begin() + take_n, cands.end(),
                              [](const Cand &a, const Cand &b) {
                                  if (a.gap != b.gap)
                                      return a.gap > b.gap;
                                  return a.id < b.id;
                              });
            chosen.assign(cands.begin(), cands.begin() + take_n);
        } else {
            std::vector<Cand> near;
            std::vector<Cand> far;
            for (auto &c : cands) {
                auto it = entities_.find(c.id);
                const int layer = it == entities_.end() ? -1 : it->second.layer;
                const int64_t pend = it == entities_.end() ? 0 : it->second.pending_since_ms;
                const int64_t age = pend > 0 ? now - pend : 0;
                c.layer = layer;
                c.hard = layer == 1 ? age >= NearHardSilenceMs() : age >= FarHardSilenceMs();
                if (layer == 1)
                    near.push_back(c);
                else
                    far.push_back(c);
            }
            int near_take = static_cast<int>(near.size());
            int far_take = static_cast<int>(far.size());
            if (!near.empty()) {
                near_take = std::max(1, limit * NearQuotaPercent() / 100);
                if (near_take > limit)
                    near_take = limit;
                if (near_take > static_cast<int>(near.size()))
                    near_take = static_cast<int>(near.size());
                far_take = limit - near_take;
                if (far_take > static_cast<int>(far.size())) {
                    near_take = std::min(static_cast<int>(near.size()),
                                         near_take + (far_take - static_cast<int>(far.size())));
                    far_take = static_cast<int>(far.size());
                }
            } else {
                near_take = 0;
                far_take = std::min(limit, static_cast<int>(far.size()));
            }
            if (near_take > 0) {
                std::partial_sort(near.begin(), near.begin() + near_take, near.end(), better);
                chosen.insert(chosen.end(), near.begin(), near.begin() + near_take);
            }
            if (far_take > 0) {
                std::partial_sort(far.begin(), far.begin() + far_take, far.end(), better);
                chosen.insert(chosen.end(), far.begin(), far.begin() + far_take);
            }
            // 人已经在槽里时必须发出预算装得下的那些。凑不满默认 8 人不能再等：
            // 满员后每条连接每秒只够大约 6 人，旧条件会把这一帧退回，并每 50ms 空转一次。
            // 只在人还没到、而且预算装得下更大的一帧时，才等一个发送间隔。
            const int batch_min = MoveBatchMinEvents();
            const int have = static_cast<int>(cands.size());
            const bool batch_wait = !any_hard && have < batch_min && have < limit &&
                                    oldest < static_cast<int64_t>(interval);
            if (batch_wait) {
                if (wait_ms) {
                    *wait_ms = static_cast<int64_t>(interval) - oldest;
                    if (*wait_ms < 50)
                        *wait_ms = 50;
                }
                ServerStats::aoi_move_batch_hold.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
        }
        const int take_n = static_cast<int>(chosen.size());
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
        any_hard_pick_ = any_hard;
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

    /** 停放帧换成更新的坐标。清掉在途标记，不记失败次数，也不重算等待时间。 */
    void NoteParkSwapped(const std::vector<AoiMoveStamp> &frame) {
        for (const auto &stamp : frame) {
            auto it = entities_.find(stamp.entity_id);
            if (it == entities_.end())
                continue;
            it->second.in_flight = false;
        }
    }

    int UnwrittenAttemptsForTest(uint64_t id) const {
        auto it = entities_.find(id);
        return it == entities_.end() ? 0 : it->second.unwritten_attempts;
    }

    int64_t PendingSinceForTest(uint64_t id) const {
        auto it = entities_.find(id);
        return it == entities_.end() ? 0 : it->second.pending_since_ms;
    }

    void NoteKernelWrite(const std::vector<AoiMoveStamp> &frame, int64_t now, int frame_bytes) {
        ServerStats::aoi_move_commit.fetch_add(1, std::memory_order_relaxed);
            ServerStats::aoi_move_park_written.fetch_add(1, std::memory_order_relaxed);
        ServerStats::aoi_move_frame_events.fetch_add(frame.size(), std::memory_order_relaxed);
        ServerStats::aoi_move_frames.fetch_add(1, std::memory_order_relaxed);
        if (any_hard_pick_) {
            ServerStats::aoi_move_force_picks.fetch_add(1, std::memory_order_relaxed);
            ServerStats::aoi_move_force_bytes.fetch_add(static_cast<uint64_t>(frame_bytes > 0 ? frame_bytes : 0),
                                                        std::memory_order_relaxed);
        }
        if (legacy_)
            return;
        if (frame_bytes > 0) {
            const int wire = frame_bytes + kOnWireHeaderBytes;
            budget_bytes_ += wire;
            charged_total_ += wire;
            ServerStats::egress_admitted_bytes.fetch_add(static_cast<uint64_t>(wire),
                                                         std::memory_order_relaxed);
            if (window_budget_ > 0 && budget_bytes_ > window_budget_) {
                const int over = budget_bytes_ - window_budget_;
                if (over > over_reported_) {
                    ServerStats::aoi_move_budget_over_bytes.fetch_add(
                        static_cast<uint64_t>(over - over_reported_), std::memory_order_relaxed);
                    over_reported_ = over;
                }
            }
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
            const char *inst_env = std::getenv("GAMEMESH_GATEWAY_INSTANCES");
            int instances = (inst_env && *inst_env) ? std::atoi(inst_env) : 2;
            if (instances < 1)
                instances = 1;
            int v = static_cast<int>(mbps * 1000000.0 / 8.0);
            v /= instances;
            if (v < 1)
                v = 1;
            ServerStats::egress_budget_bytes_per_sec.store(v, std::memory_order_relaxed);
            return v;
        }();
        return bytes;
    }

private:
    bool legacy_ = false;
    int coalesce_interval_ms_ = 200;
    int quota_numerator_ = 100;
    int window_budget_ = 0;
    int over_reported_ = 0;
    int charged_total_ = 0;
    bool any_hard_pick_ = false;
    std::unordered_map<uint64_t, Entity> entities_;
    int64_t last_written_flush_ms_ = 0;
    int64_t budget_window_ms_ = 0;
    int budget_bytes_ = 0;
};
