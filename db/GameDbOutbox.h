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

    bool FetchUnpublished(int limit, std::vector<GameDbOutboxRow> *out);
    bool MarkPublished(uint64_t id, int64_t published_at);

    /** 测试：按幂等键计数 */
    int CountByIdempotency(const std::string &idempotency_key);

private:
    GameDbOutbox() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
