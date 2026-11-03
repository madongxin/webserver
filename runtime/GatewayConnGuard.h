#pragma once

#include <cstdint>
#include <string>

/** 连接级 Hello/心跳/限流状态。禁止在此访问 Redis/MySQL。 */
class GatewayConnGuard {
public:
    static GatewayConnGuard &Instance();

    void OnConnected(uint64_t conn_id, int fd);
    void OnDisconnected(uint64_t conn_id);
    void NoteActivity(uint64_t conn_id, size_t frame_bytes);

    bool HelloOk(uint64_t conn_id) const;
    void SetHelloOk(uint64_t conn_id, bool ok);

    /** 超限返回错误码；空串表示通过。 */
    std::string CheckConnectRate(int fd);
    std::string CheckFrameRate(uint64_t conn_id, size_t frame_bytes);
    std::string CheckHeartbeatRate(uint64_t conn_id, bool bound);
    std::string CheckAuthCommandRate(uint64_t conn_id);
    std::string CheckChatRate(uint64_t conn_id);
    std::string CheckNameQueryRate(uint64_t conn_id);

    bool IdleExpired(uint64_t conn_id, uint32_t idle_ms) const;
    /** 限流错误回复每条连接每秒最多放行一次。 */
    bool AllowRateError(uint64_t conn_id);
    /** 连续超限达到 ms 则应关连接。 */
    bool RateLimitedTooLong(uint64_t conn_id, int64_t ms);
    /** 服务端暂停读的时间不计入空闲超时。 */
    void AddIdleCredit(uint64_t conn_id, int64_t paused_ms);
    std::string PeerIp(uint64_t conn_id) const;

private:
    GatewayConnGuard() = default;
};
