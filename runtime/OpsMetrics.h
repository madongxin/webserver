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
    void IncIdentityMismatch() { identity_mismatch_.fetch_add(1, std::memory_order_relaxed); }

    void SetOnlinePlayers(int64_t n) { online_players_.store(n, std::memory_order_relaxed); }
    void SetOutboxBacklog(int64_t n) { outbox_backlog_.store(n, std::memory_order_relaxed); }

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
    std::atomic<uint64_t> identity_mismatch_{0};
    std::atomic<int64_t> online_players_{0};
    std::atomic<int64_t> outbox_backlog_{0};
};
