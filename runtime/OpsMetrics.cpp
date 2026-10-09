#include "OpsMetrics.h"

#include "ServerStats.h"

#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {
std::mutex g_err_mu;
std::unordered_map<std::string, uint64_t> g_err_counts;
}  // namespace

OpsMetrics &OpsMetrics::Instance() {
    static OpsMetrics g;
    return g;
}

void OpsMetrics::IncErrorCode(const std::string &code) {
    const std::string key = code.empty() ? "ERR_INTERNAL" : code;
    std::lock_guard<std::mutex> lk(g_err_mu);
    g_err_counts[key] += 1;
}

std::string OpsMetrics::PrometheusText() const {
    std::ostringstream os;
    auto ctr = [&](const char *name, const char *help, uint64_t v) {
        os << "# HELP " << name << " " << help << "\n# TYPE " << name << " counter\n"
           << name << " " << v << "\n\n";
    };
    auto gauge = [&](const char *name, const char *help, int64_t v) {
        os << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n"
           << name << " " << v << "\n\n";
    };
    ctr("gamemesh_tcp_connect_total", "Accepted TCP connections.",
        tcp_connect_.load(std::memory_order_relaxed));
    ctr("gamemesh_tcp_disconnect_total", "TCP disconnects.",
        tcp_disconnect_.load(std::memory_order_relaxed));
    ctr("gamemesh_tcp_illegal_frame_total", "Invalid protocol frames.",
        illegal_frame_.load(std::memory_order_relaxed));
    ctr("gamemesh_login_ok_total", "Login successes.", login_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_login_fail_total", "Login failures.",
        login_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_reconnect_ok_total", "Reconnect successes.",
        reconnect_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_reconnect_fail_total", "Reconnect failures.",
        reconnect_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_register_ok_total", "Register successes.",
        register_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_register_fail_total", "Register failures.",
        register_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_fence_reject_total", "Session fence rejects.",
        fence_reject_.load(std::memory_order_relaxed));
    ctr("gamemesh_drain_reject_total", "Rejected while draining.",
        drain_reject_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_accepted_total", "Push accepted.",
        push_accepted_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_rejected_total", "Push rejected.",
        push_rejected_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_ack_ok_total", "Reliable push ACK ok.",
        push_ack_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_ack_duplicate_total", "Reliable push ACK duplicate.",
        push_ack_duplicate_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_ack_ahead_rejected_total", "Reliable push ACK ahead of server_seq.",
        push_ack_ahead_rejected_.load(std::memory_order_relaxed));
    ctr("gamemesh_push_ack_stale_rejected_total", "Reliable push ACK behind last_ack.",
        push_ack_stale_rejected_.load(std::memory_order_relaxed));
    ctr("gamemesh_identity_mismatch_total", "Client player_id mismatch vs trusted binding.",
        identity_mismatch_.load(std::memory_order_relaxed));
    ctr("gamemesh_queue_overload_total", "PlayerSerialQueue TryPost rejected.",
        queue_overload_.load(std::memory_order_relaxed));
    ctr("gamemesh_command_forbidden_total", "Client command policy rejects.",
        command_forbidden_.load(std::memory_order_relaxed));
    ctr("gamemesh_hello_ok_total", "ClientHello successes.",
        hello_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_hello_fail_total", "ClientHello failures.",
        hello_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_heartbeat_ok_total", "Heartbeat successes.",
        heartbeat_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_heartbeat_limited_total", "Heartbeat rate-limited.",
        heartbeat_limited_.load(std::memory_order_relaxed));
    ctr("gamemesh_idle_timeout_total", "TCP idle timeout closes.",
        idle_timeout_.load(std::memory_order_relaxed));
    ctr("gamemesh_conn_rate_limited_total", "Per-IP connect rate rejects.",
        conn_rate_limited_.load(std::memory_order_relaxed));
    {
        std::lock_guard<std::mutex> lk(g_err_mu);
        os << "# HELP gamemesh_error_code_total Public error_code responses.\n"
              "# TYPE gamemesh_error_code_total counter\n";
        for (const auto &kv : g_err_counts)
            os << "gamemesh_error_code_total{code=\"" << kv.first << "\"} " << kv.second << "\n";
        os << "\n";
    }
    gauge("gamemesh_online_players", "Bound unique players on this Gateway.",
          online_players_.load(std::memory_order_relaxed));
    gauge("gamemesh_session_online_players",
          "Redis Session ONLINE set size (meaningful on role=session).",
          session_online_players_.load(std::memory_order_relaxed));
    gauge("gamemesh_gateway_tcp_connections", "Current game TCP sockets (including pre-auth).",
          gateway_tcp_connections_.load(std::memory_order_relaxed));
    ctr("gamemesh_gateway_rx_bytes_total", "Game TCP bytes received (client → Gateway).",
        gateway_rx_bytes_.load(std::memory_order_relaxed));
    ctr("gamemesh_gateway_tx_bytes_total", "Game TCP bytes sent (Gateway → client).",
        gateway_tx_bytes_.load(std::memory_order_relaxed));
    ctr("gamemesh_gateway_rx_frames_total", "Game TCP frames received.",
        gateway_rx_frames_.load(std::memory_order_relaxed));
    ctr("gamemesh_gateway_tx_frames_total", "Game TCP frames sent.",
        gateway_tx_frames_.load(std::memory_order_relaxed));
    gauge("gamemesh_outbox_backlog", "Unpublished outbox rows (GameDB).",
          outbox_backlog_.load(std::memory_order_relaxed));
    gauge("gamemesh_mailbox_pending", "PlayerSerialQueue pending_global.",
          mailbox_pending_.load(std::memory_order_relaxed));
    ctr("gamemesh_placement_recover_ok_total", "Placement auto recover success.",
        placement_recover_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_placement_recover_fail_total", "Placement auto recover failure.",
        placement_recover_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_gamedb_unknown_result_total", "GameDB write unknown-result outcomes.",
        gamedb_unknown_result_.load(std::memory_order_relaxed));
    ctr("gamemesh_logic_discover_not_ready_total", "GameLogic discover skipped (registry not ready).",
        logic_discover_not_ready_.load(std::memory_order_relaxed));
    ctr("gamemesh_logic_discover_fail_total", "GameLogic discover RPC/IO failed (snapshot kept).",
        logic_discover_fail_.load(std::memory_order_relaxed));
    ctr("gamemesh_logic_discover_empty_total", "GameLogic discover succeeded with zero instances.",
        logic_discover_empty_.load(std::memory_order_relaxed));
    ctr("gamemesh_logic_discover_ok_total", "GameLogic discover succeeded nonempty.",
        logic_discover_ok_.load(std::memory_order_relaxed));
    ctr("gamemesh_disconnect_accepted_total", "Disconnect mark accepted (queue or rpc).",
        disconnect_accepted_.load(std::memory_order_relaxed));
    ctr("gamemesh_disconnect_dropped_total", "Disconnect mark dropped after fallback fail.",
        disconnect_dropped_.load(std::memory_order_relaxed));
    ctr("gamemesh_disconnect_retried_total", "Disconnect mark rpc fallback after queue full.",
        disconnect_retried_.load(std::memory_order_relaxed));
    ctr("gamemesh_disconnect_compensated_total", "Disconnect mark executed by async worker.",
        disconnect_compensated_.load(std::memory_order_relaxed));
    ctr("gamemesh_disconnect_failed_total", "Disconnect mark failed.",
        disconnect_failed_.load(std::memory_order_relaxed));
    ctr("gamemesh_kick_gateway_attempt_total", "Session Kick best-effort GatewayKick.",
        kick_gateway_attempt_.load(std::memory_order_relaxed));
    ctr("gamemesh_move_err_stale_seq_total", "Move or dispatch rejected with ERR_STALE_SEQ.",
        ServerStats::move_err_stale_seq.load(std::memory_order_relaxed));
    ctr("gamemesh_session_grace_expire_total", "Disconnected sessions past grace.",
        ServerStats::session_grace_expire.load(std::memory_order_relaxed));
    ctr("gamemesh_player_transfer_abort_total", "Player transfers rolled back after TTL.",
        ServerStats::player_transfer_abort.load(std::memory_order_relaxed));
    ctr("gamemesh_route_apply_rejected_total", "Gateway route updates dropped as stale.",
        ServerStats::route_apply_rejected.load(std::memory_order_relaxed));
    ctr("gamemesh_session_replace_total", "Connections notified for session replace.",
        ServerStats::session_replace_count.load(std::memory_order_relaxed));
    ctr("gamemesh_session_replace_notify_failed_total", "Session replace notify had no frame.",
        ServerStats::session_replace_notify_failed.load(std::memory_order_relaxed));
    ctr("gamemesh_redis_lua_errors_total", "Redis Lua EVAL returned an error.",
        ServerStats::redis_lua_errors.load(std::memory_order_relaxed));
    os << "# HELP gamemesh_friend_request_total Friend apply results.\n"
          "# TYPE gamemesh_friend_request_total counter\n";
    os << "gamemesh_friend_request_total{result=\"ok\"} "
       << ServerStats::friend_request_ok.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_request_total{result=\"denied\"} "
       << ServerStats::friend_request_denied.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_request_total{result=\"error\"} "
       << ServerStats::friend_request_error.load(std::memory_order_relaxed) << "\n";
    ctr("gamemesh_friend_accept_total", "Friend accepts that committed.",
        ServerStats::friend_accept_total.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_delete_total", "Friend deletes that committed.",
        ServerStats::friend_delete_total.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_block_total", "Friend blocks that committed.",
        ServerStats::friend_block_total.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_gamedb_error_total", "Friend GameDB calls that failed.",
        ServerStats::friend_gamedb_error.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_transaction_conflict_total", "Friend ops that hit ERR_RELATION_CONFLICT.",
        ServerStats::friend_transaction_conflict.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_replay_append_fail_total", "Reliable friend push could not be stored.",
        ServerStats::friend_replay_append_fail.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_cache_hit_total", "Friend id cache hits.",
        ServerStats::friend_cache_hit.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_cache_miss_total", "Friend id cache misses.",
        ServerStats::friend_cache_miss.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_block_gate_cache_hit_total", "Whisper block cache hits.",
        ServerStats::friend_block_gate_cache_hit.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_block_gate_cache_miss_total", "Whisper block cache misses.",
        ServerStats::friend_block_gate_cache_miss.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_block_gate_degraded_total", "Whisper allowed because the block cache was down.",
        ServerStats::friend_block_gate_degraded.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_presence_queue_drop_total", "Presence updates dropped because the queue was full.",
        ServerStats::friend_presence_queue_drop.load(std::memory_order_relaxed));
    ctr("gamemesh_friend_presence_coalesced_total", "Presence updates merged inside the window.",
        ServerStats::friend_presence_coalesced.load(std::memory_order_relaxed));
    os << "# HELP gamemesh_friend_presence_batch_size Last presence PushBatch size.\n"
          "# TYPE gamemesh_friend_presence_batch_size gauge\n";
    os << "gamemesh_friend_presence_batch_size "
       << ServerStats::friend_presence_batch_size.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_friend_push_success Friend pushes accepted by a gateway.\n"
          "# TYPE gamemesh_friend_push_success counter\n";
    os << "gamemesh_friend_push_success{type=\"request\"} "
       << ServerStats::friend_push_ok_request.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_success{type=\"added\"} "
       << ServerStats::friend_push_ok_added.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_success{type=\"removed\"} "
       << ServerStats::friend_push_ok_removed.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_success{type=\"presence\"} "
       << ServerStats::friend_push_ok_presence.load(std::memory_order_relaxed) << "\n";
    os << "# HELP gamemesh_friend_push_fail Friend pushes a gateway rejected.\n"
          "# TYPE gamemesh_friend_push_fail counter\n";
    os << "gamemesh_friend_push_fail{type=\"request\"} "
       << ServerStats::friend_push_fail_request.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_fail{type=\"added\"} "
       << ServerStats::friend_push_fail_added.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_fail{type=\"removed\"} "
       << ServerStats::friend_push_fail_removed.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_push_fail{type=\"presence\"} "
       << ServerStats::friend_push_fail_presence.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_list_latency_ms_sum "
       << ServerStats::friend_list_latency_ms_sum.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_list_latency_count "
       << ServerStats::friend_list_latency_count.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_gamedb_latency_ms_sum "
       << ServerStats::friend_gamedb_latency_ms_sum.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_gamedb_latency_count "
       << ServerStats::friend_gamedb_latency_count.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_online_batch_latency_ms_sum "
       << ServerStats::friend_online_batch_latency_ms_sum.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_friend_online_batch_latency_count "
       << ServerStats::friend_online_batch_latency_count.load(std::memory_order_relaxed) << "\n";
    ctr("gamemesh_db_pool_exhausted_total", "MySQL pool wait timed out with no connection.",
        ServerStats::db_pool_exhausted.load(std::memory_order_relaxed));
    os << "# HELP gamemesh_db_pool_wait_ms MySQL connection checkout latency.\n"
          "# TYPE gamemesh_db_pool_wait_ms histogram\n";
    auto bucket = [&](const char *le, uint64_t v) {
        os << "gamemesh_db_pool_wait_ms_bucket{le=\"" << le << "\"} " << v << "\n";
    };
    bucket("1", ServerStats::db_wait_le_1.load(std::memory_order_relaxed));
    bucket("5", ServerStats::db_wait_le_5.load(std::memory_order_relaxed));
    bucket("10", ServerStats::db_wait_le_10.load(std::memory_order_relaxed));
    bucket("50", ServerStats::db_wait_le_50.load(std::memory_order_relaxed));
    bucket("100", ServerStats::db_wait_le_100.load(std::memory_order_relaxed));
    bucket("500", ServerStats::db_wait_le_500.load(std::memory_order_relaxed));
    bucket("1000", ServerStats::db_wait_le_1000.load(std::memory_order_relaxed));
    bucket("+Inf", ServerStats::db_wait_le_inf.load(std::memory_order_relaxed));
    os << "gamemesh_db_pool_wait_ms_sum "
       << ServerStats::db_pool_wait_sum_ms.load(std::memory_order_relaxed) << "\n";
    os << "gamemesh_db_pool_wait_ms_count "
       << ServerStats::db_pool_wait_count.load(std::memory_order_relaxed) << "\n\n";
    return os.str();
}
