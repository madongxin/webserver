#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

/**
 * 每连接 Login/Reconnect 流程代次：
 * - 同一连接同时只允许一个 in-flight 编排
 * - 回调时校验 connection 仍存活且 flow_gen 匹配，防止迟到回调写脏绑定
 */
class GatewayAuthFlow {
public:
    static GatewayAuthFlow &Instance();

    void OnConnected(uint64_t connection_id);
    void OnDisconnected(uint64_t connection_id);

    /** 开始编排；若已有 in-flight 返回 false */
    bool TryBegin(uint64_t connection_id, uint64_t *flow_gen_out);
    void End(uint64_t connection_id, uint64_t flow_gen);

    /** 回调前校验：连接仍存在且 generation 匹配 */
    bool AcceptCallback(uint64_t connection_id, uint64_t flow_gen) const;
    bool Alive(uint64_t connection_id) const;

private:
    struct Slot {
        bool alive = false;
        bool in_flight = false;
        uint64_t flow_gen = 0;
    };
    GatewayAuthFlow() = default;
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, Slot> slots_;
};
