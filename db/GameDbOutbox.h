#pragma once

/**
 * @file GameDbOutbox.h
 * @brief 本地 outbox 表：与资产事务同提交，后台发布（阶段 4 先落库+标记，不上 NATS）
 */

#include <cstdint>
#include <string>
#include <vector>

class Connection;

struct GameDbOutboxRow {
    uint64_t id = 0;
    std::string event_type;
    std::string aggregate_type;
    std::string aggregate_id;
    std::string idempotency_key;
    std::string payload;
    int64_t created_at = 0;
    int64_t published_at = 0;
};

class GameDbOutbox {
public:
    static GameDbOutbox &Instance();

    bool EnsureTable();
    bool Available() const { return available_; }

    bool InsertOnConnection(Connection *conn, const std::string &event_type,
                            const std::string &aggregate_type, const std::string &aggregate_id,
                            const std::string &idempotency_key, const std::string &payload,
                            int64_t created_at);

    /** 只读拉取（单测/诊断）；多 publisher 请用 ClaimUnpublished */
    bool FetchUnpublished(int limit, std::vector<GameDbOutboxRow> *out);

    /**
     * 认领未发布行：FOR UPDATE SKIP LOCKED，并将 published_at 置为负 sentinel（进行中）。
     * 发布成功后 MarkPublished；失败调用 ReleaseClaim。
     */
    bool ClaimUnpublished(int limit, std::vector<GameDbOutboxRow> *out);

    bool MarkPublished(uint64_t id, int64_t published_at);
    bool ReleaseClaim(uint64_t id);

    /** 测试：按幂等键计数 */
    int CountByIdempotency(const std::string &idempotency_key);

    /** 未发布（含 claim 中）积压：published_at IS NULL OR published_at < 0 */
    int CountUnpublished();

    /** published_at 进行中标记（负值，避免与真实 unix 时间戳冲突） */
    static constexpr int64_t kClaimSentinel = -1;

private:
    GameDbOutbox() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
