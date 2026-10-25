#pragma once

#include <atomic>
#include <cstdint>

/** 进程内计数。头文件内联，避免网关单测再链一套指标对象。 */
struct ServerStats {
    static inline std::atomic<uint64_t> move_err_stale_seq{0};
    /** Move 请求超速被钳到本帧合法距离（仍接受并推送）。 */
    static inline std::atomic<uint64_t> move_speed_clamp{0};
    /** Move 落点不可走，沿方向收到可行走格后仍接受。 */
    static inline std::atomic<uint64_t> move_walk_slide{0};
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

    static inline std::atomic<uint64_t> aoi_candidate_sum{0};
    static inline std::atomic<uint64_t> aoi_candidate_count{0};
    static inline std::atomic<uint64_t> aoi_selected_sum{0};
    static inline std::atomic<uint64_t> aoi_visible_query_us_sum{0};
    static inline std::atomic<uint64_t> aoi_visible_query_count{0};
    static inline std::atomic<uint64_t> aoi_query_le_100{0};
    static inline std::atomic<uint64_t> aoi_query_le_500{0};
    static inline std::atomic<uint64_t> aoi_query_le_2000{0};
    static inline std::atomic<uint64_t> aoi_query_le_inf{0};
    static inline std::atomic<uint64_t> aoi_refresh_us_sum{0};
    static inline std::atomic<uint64_t> aoi_refresh_count{0};
    static inline std::atomic<uint64_t> aoi_refresh_viewers_sum{0};
    static inline std::atomic<uint64_t> aoi_sub_enter{0};
    static inline std::atomic<uint64_t> aoi_sub_leave{0};
    static inline std::atomic<uint64_t> aoi_move_events{0};
    static inline std::atomic<uint64_t> aoi_fast_move{0};
    static inline std::atomic<uint64_t> aoi_move_rate_limited{0};
    static inline std::atomic<uint64_t> aoi_move_capped{0};
    static inline std::atomic<uint64_t> aoi_move_buffer_hold{0};
    static inline std::atomic<uint64_t> aoi_move_budget_defer{0};
    static inline std::atomic<uint64_t> aoi_move_overwritten{0};
    /** 进程启动以来，一个实体的最新位置在合并槽里等待的最长时间。 */
    static inline std::atomic<uint64_t> aoi_move_pending_oldest_ms{0};
    /** 进程启动以来，同一观察者对同一实体两次 MOVE 写出的最大间隔。 */
    static inline std::atomic<uint64_t> aoi_move_entity_max_silence_ms{0};
    /** 进程启动以来，最新 MOVE 帧因为内核发送队列而停在应用缓冲里的最长时间。 */
    static inline std::atomic<uint64_t> latest_move_hold_ms{0};
    static inline std::atomic<uint64_t> aoi_lock_wait_us_sum{0};
    static inline std::atomic<uint64_t> aoi_lock_hold_us_sum{0};
    static inline std::atomic<uint64_t> aoi_lock_samples{0};
    static inline std::atomic<uint64_t> login_stage_reject_auth{0};
    static inline std::atomic<uint64_t> login_stage_reject_session{0};
    static inline std::atomic<uint64_t> login_stage_reject_bind{0};
    static inline std::atomic<uint64_t> login_stage_reject_enter{0};

    static void ObserveAoiQueryUs(uint64_t us) {
        aoi_visible_query_us_sum.fetch_add(us, std::memory_order_relaxed);
        aoi_visible_query_count.fetch_add(1, std::memory_order_relaxed);
        aoi_query_le_inf.fetch_add(1, std::memory_order_relaxed);
        if (us <= 2000)
            aoi_query_le_2000.fetch_add(1, std::memory_order_relaxed);
        if (us <= 500)
            aoi_query_le_500.fetch_add(1, std::memory_order_relaxed);
        if (us <= 100)
            aoi_query_le_100.fetch_add(1, std::memory_order_relaxed);
    }

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
