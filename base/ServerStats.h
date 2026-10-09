#pragma once

#include <atomic>
#include <cstdint>

/** 进程内计数。头文件内联，避免网关单测再链一套指标对象。 */
struct ServerStats {
    static inline std::atomic<uint64_t> move_err_stale_seq{0};
    static inline std::atomic<uint64_t> session_grace_expire{0};
    static inline std::atomic<uint64_t> player_transfer_abort{0};
    static inline std::atomic<uint64_t> route_apply_rejected{0};
    static inline std::atomic<uint64_t> session_replace_count{0};
    static inline std::atomic<uint64_t> session_replace_notify_failed{0};
    static inline std::atomic<uint64_t> redis_lua_errors{0};
    static inline std::atomic<uint64_t> db_pool_exhausted{0};
    static inline std::atomic<uint64_t> db_pool_wait_count{0};
    static inline std::atomic<uint64_t> db_pool_wait_sum_ms{0};
    static inline std::atomic<uint64_t> db_wait_le_1{0};
    static inline std::atomic<uint64_t> db_wait_le_5{0};
    static inline std::atomic<uint64_t> db_wait_le_10{0};
    static inline std::atomic<uint64_t> db_wait_le_50{0};
    static inline std::atomic<uint64_t> db_wait_le_100{0};
    static inline std::atomic<uint64_t> db_wait_le_500{0};
    static inline std::atomic<uint64_t> db_wait_le_1000{0};
    static inline std::atomic<uint64_t> db_wait_le_inf{0};

    static inline std::atomic<uint64_t> friend_request_ok{0};
    static inline std::atomic<uint64_t> friend_request_denied{0};
    static inline std::atomic<uint64_t> friend_request_error{0};
    static inline std::atomic<uint64_t> friend_accept_total{0};
    static inline std::atomic<uint64_t> friend_delete_total{0};
    static inline std::atomic<uint64_t> friend_block_total{0};
    static inline std::atomic<uint64_t> friend_list_latency_ms_sum{0};
    static inline std::atomic<uint64_t> friend_list_latency_count{0};
    static inline std::atomic<uint64_t> friend_gamedb_latency_ms_sum{0};
    static inline std::atomic<uint64_t> friend_gamedb_latency_count{0};
    static inline std::atomic<uint64_t> friend_gamedb_error{0};
    static inline std::atomic<uint64_t> friend_online_batch_latency_ms_sum{0};
    static inline std::atomic<uint64_t> friend_online_batch_latency_count{0};
    static inline std::atomic<uint64_t> friend_push_ok_request{0};
    static inline std::atomic<uint64_t> friend_push_ok_added{0};
    static inline std::atomic<uint64_t> friend_push_ok_removed{0};
    static inline std::atomic<uint64_t> friend_push_ok_presence{0};
    static inline std::atomic<uint64_t> friend_push_fail_request{0};
    static inline std::atomic<uint64_t> friend_push_fail_added{0};
    static inline std::atomic<uint64_t> friend_push_fail_removed{0};
    static inline std::atomic<uint64_t> friend_push_fail_presence{0};
    static inline std::atomic<uint64_t> friend_replay_append_fail{0};
    static inline std::atomic<uint64_t> friend_cache_hit{0};
    static inline std::atomic<uint64_t> friend_cache_miss{0};
    static inline std::atomic<uint64_t> friend_transaction_conflict{0};
    static inline std::atomic<uint64_t> friend_block_gate_cache_hit{0};
    static inline std::atomic<uint64_t> friend_block_gate_cache_miss{0};
    static inline std::atomic<uint64_t> friend_block_gate_degraded{0};
    static inline std::atomic<uint64_t> friend_presence_queue_drop{0};
    static inline std::atomic<uint64_t> friend_presence_coalesced{0};
    static inline std::atomic<uint64_t> friend_presence_batch_size{0};

    static void ObserveDbWaitMs(uint64_t ms) {
        db_pool_wait_count.fetch_add(1, std::memory_order_relaxed);
        db_pool_wait_sum_ms.fetch_add(ms, std::memory_order_relaxed);
        db_wait_le_inf.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 1000)
            db_wait_le_1000.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 500)
            db_wait_le_500.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 100)
            db_wait_le_100.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 50)
            db_wait_le_50.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 10)
            db_wait_le_10.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 5)
            db_wait_le_5.fetch_add(1, std::memory_order_relaxed);
        if (ms <= 1)
            db_wait_le_1.fetch_add(1, std::memory_order_relaxed);
    }
};
