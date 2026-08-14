#include "OpsMetrics.h"

#include <sstream>

OpsMetrics &OpsMetrics::Instance() {
    static OpsMetrics g;
    return g;
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
    gauge("gamemesh_online_players", "Bound gateway connections (approx).",
          online_players_.load(std::memory_order_relaxed));
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
    return os.str();
}
