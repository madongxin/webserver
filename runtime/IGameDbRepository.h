#pragma once

/**
 * @file IGameDbRepository.h
 * @brief GameDB 异步仓储接口（阶段 4）
 *
 * SQL 只在 GameDB worker / 连接池线程执行；业务线程通过回调或同步等待取结果。
 * 禁止在 Reactor IO 线程调用同步等待版本。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct GameDbGrantedItem {
    uint64_t asset_id = 0;
    uint32_t count = 0;
};

struct GameDbMailClaimRequest {
    uint64_t player_id = 0;
    uint64_t mail_id = 0;
    std::string idempotency_key;
    std::string trace_id;
    int64_t inventory_soft_cap = 999999;
    /** 提交时的内存背包快照，供 worker 内软上限校验（不在 DB 线程读 GameLogic） */
    std::unordered_map<uint32_t, uint32_t> bag_snapshot;
};

struct GameDbMailClaimResult {
    bool ok = false;
    bool idempotent_hit = false;
    /** 仅首次事务成功入账时为 true；幂等命中为 false，避免重复改内存 */
    bool should_apply_memory = false;
    std::string error_code;
    std::string message;
    std::string attachment_state;
    int64_t mail_row_version = 0;
    std::vector<GameDbGrantedItem> grants;
    /** MySQL 提交后的 asset_version；幂等命中为该次操作已提交版本 */
    uint64_t asset_version = 0;
};

class IGameDbRepository {
public:
    virtual ~IGameDbRepository() = default;

    virtual void Start(int worker_count = 2) = 0;
    virtual void Stop() = 0;
    virtual bool started() const = 0;

    using MailClaimDone = std::function<void(GameDbMailClaimResult)>;

    /** 异步领取：完成回调在 GameDB worker 线程触发 */
    virtual void ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) = 0;

    /** 同步领取（MailBrpc/单测；禁止在 Reactor IO / 玩家串行线程调用） */
    virtual GameDbMailClaimResult ClaimMailAttachments(GameDbMailClaimRequest req) = 0;
};
