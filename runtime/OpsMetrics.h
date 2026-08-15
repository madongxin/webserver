#pragma once

#include <atomic>
#include <cstdint>
#include <string>

/** 阶段 7：进程内运维计数（Prometheus 导出；不要求跨实例聚合）。 */
class OpsMetrics {
public:
    static OpsMetrics &Instance();

    void IncTcpConnect() { tcp_connect_.fetch_add(1, std::memory_order_relaxed); }
    void IncTcpDisconnect() { tcp_disconnect_.fetch_add(1, std::memory_order_relaxed); }
    void IncIllegalFrame() { illegal_frame_.fetch_add(1, std::memory_order_relaxed); }

    void IncLoginOk() { login_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncLoginFail() { login_fail_.fetch_add(1, std::memory_order_relaxed); }
    void IncReconnectOk() { reconnect_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncReconnectFail() { reconnect_fail_.fetch_add(1, std::memory_order_relaxed); }
    void IncRegisterOk() { register_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncRegisterFail() { register_fail_.fetch_add(1, std::memory_order_relaxed); }

    void IncFenceReject() { fence_reject_.fetch_add(1, std::memory_order_relaxed); }
    void IncDrainReject() { drain_reject_.fetch_add(1, std::memory_order_relaxed); }
    void IncPushAccepted() { push_accepted_.fetch_add(1, std::memory_order_relaxed); }
    void IncPushRejected() { push_rejected_.fetch_add(1, std::memory_order_relaxed); }
    void IncPushAckOk() { push_ack_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncPushAckDuplicate() { push_ack_duplicate_.fetch_add(1, std::memory_order_relaxed); }
    void IncPushAckAheadRejected() {
        push_ack_ahead_rejected_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncPushAckStaleRejected() {
        push_ack_stale_rejected_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncIdentityMismatch() { identity_mismatch_.fetch_add(1, std::memory_order_relaxed); }
    void IncQueueOverload() { queue_overload_.fetch_add(1, std::memory_order_relaxed); }
    void IncCommandForbidden() { command_forbidden_.fetch_add(1, std::memory_order_relaxed); }
    void IncHelloOk() { hello_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncHelloFail() { hello_fail_.fetch_add(1, std::memory_order_relaxed); }
    void IncHeartbeatOk() { heartbeat_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncHeartbeatLimited() { heartbeat_limited_.fetch_add(1, std::memory_order_relaxed); }
    void IncIdleTimeout() { idle_timeout_.fetch_add(1, std::memory_order_relaxed); }
    void IncConnRateLimited() { conn_rate_limited_.fetch_add(1, std::memory_order_relaxed); }
    void IncErrorCode(const std::string &code);

    void SetOnlinePlayers(int64_t n) { online_players_.store(n, std::memory_order_relaxed); }
    void SetOutboxBacklog(int64_t n) { outbox_backlog_.store(n, std::memory_order_relaxed); }
    void SetMailboxPending(int64_t n) { mailbox_pending_.store(n, std::memory_order_relaxed); }
    void IncPlacementRecoverOk() { placement_recover_ok_.fetch_add(1, std::memory_order_relaxed); }
    void IncPlacementRecoverFail() {
        placement_recover_fail_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncGamedbUnknownResult() {
        gamedb_unknown_result_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncLogicDiscoverNotReady() {
        logic_discover_not_ready_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncLogicDiscoverFail() {
        logic_discover_fail_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncLogicDiscoverEmpty() {
        logic_discover_empty_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncLogicDiscoverOk() { logic_discover_ok_.fetch_add(1, std::memory_order_relaxed); }

    void IncDisconnectAccepted() { disconnect_accepted_.fetch_add(1, std::memory_order_relaxed); }
    void IncDisconnectDropped() { disconnect_dropped_.fetch_add(1, std::memory_order_relaxed); }
    void IncDisconnectRetried() { disconnect_retried_.fetch_add(1, std::memory_order_relaxed); }
    void IncDisconnectCompensated() {
        disconnect_compensated_.fetch_add(1, std::memory_order_relaxed);
    }
    void IncDisconnectFailed() { disconnect_failed_.fetch_add(1, std::memory_order_relaxed); }
    void IncKickGatewayAttempt() { kick_gateway_attempt_.fetch_add(1, std::memory_order_relaxed); }

    uint64_t logic_discover_fail() const {
        return logic_discover_fail_.load(std::memory_order_relaxed);
    }
    uint64_t logic_discover_empty() const {
        return logic_discover_empty_.load(std::memory_order_relaxed);
    }

    std::string PrometheusText() const;

private:
    OpsMetrics() = default;
    std::atomic<uint64_t> tcp_connect_{0};
    std::atomic<uint64_t> tcp_disconnect_{0};
    std::atomic<uint64_t> illegal_frame_{0};
    std::atomic<uint64_t> login_ok_{0};
    std::atomic<uint64_t> login_fail_{0};
    std::atomic<uint64_t> reconnect_ok_{0};
    std::atomic<uint64_t> reconnect_fail_{0};
    std::atomic<uint64_t> register_ok_{0};
    std::atomic<uint64_t> register_fail_{0};
    std::atomic<uint64_t> fence_reject_{0};
    std::atomic<uint64_t> drain_reject_{0};
    std::atomic<uint64_t> push_accepted_{0};
    std::atomic<uint64_t> push_rejected_{0};
    std::atomic<uint64_t> push_ack_ok_{0};
    std::atomic<uint64_t> push_ack_duplicate_{0};
    std::atomic<uint64_t> push_ack_ahead_rejected_{0};
    std::atomic<uint64_t> push_ack_stale_rejected_{0};
    std::atomic<uint64_t> identity_mismatch_{0};
    std::atomic<uint64_t> queue_overload_{0};
    std::atomic<uint64_t> command_forbidden_{0};
    std::atomic<uint64_t> hello_ok_{0};
    std::atomic<uint64_t> hello_fail_{0};
    std::atomic<uint64_t> heartbeat_ok_{0};
    std::atomic<uint64_t> heartbeat_limited_{0};
    std::atomic<uint64_t> idle_timeout_{0};
    std::atomic<uint64_t> conn_rate_limited_{0};
    std::atomic<int64_t> online_players_{0};
    std::atomic<int64_t> outbox_backlog_{0};
    std::atomic<int64_t> mailbox_pending_{0};
    std::atomic<uint64_t> placement_recover_ok_{0};
    std::atomic<uint64_t> placement_recover_fail_{0};
    std::atomic<uint64_t> gamedb_unknown_result_{0};
    std::atomic<uint64_t> logic_discover_not_ready_{0};
    std::atomic<uint64_t> logic_discover_fail_{0};
    std::atomic<uint64_t> logic_discover_empty_{0};
    std::atomic<uint64_t> logic_discover_ok_{0};
    std::atomic<uint64_t> disconnect_accepted_{0};
    std::atomic<uint64_t> disconnect_dropped_{0};
    std::atomic<uint64_t> disconnect_retried_{0};
    std::atomic<uint64_t> disconnect_compensated_{0};
    std::atomic<uint64_t> disconnect_failed_{0};
    std::atomic<uint64_t> kick_gateway_attempt_{0};
};
