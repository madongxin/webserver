#pragma once

#include <functional>

/**
 * 有界、可停止的 RPC 卸载线程池。禁止 detach。
 * 用于 Gateway Login/Reconnect/EnterMap，避免阻塞 PlayerSerialQueue shard。
 */
class RpcOffloadPool {
public:
    static RpcOffloadPool &Instance();

    /** n<=0 时按硬件并发估算（至少 2） */
    void Start(int n = 0);
    void Stop();

    /** 队列满或未启动返回 false */
    bool TryPost(std::function<void()> fn);

    bool started() const;

private:
    RpcOffloadPool() = default;
    ~RpcOffloadPool();
    RpcOffloadPool(const RpcOffloadPool &) = delete;
    RpcOffloadPool &operator=(const RpcOffloadPool &) = delete;
};
