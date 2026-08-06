#pragma once

/**
 * @file PlayerItemStore.h
 * @brief 玩家道具 MySQL 持久化层（表 player_item）
 *
 * 职责：建表、单条 INSERT。实际业务写入由 PlayerItemPersistQueue 定时/登出批量调用，
 *       不在 grant_item 时直接 Insert。
 *
 * 表结构见 config/create_player_item.sql，库名来自 config/mysql.cnf 的 dbname（默认 metrics）。
 */

#include <cstdint>
#include <string>

class PlayerItemStore {
public:
    static PlayerItemStore &Instance();

    /** 连接池已初始化且至少成功建表过一次 */
    bool Available() const { return available_; }

    /** CREATE TABLE IF NOT EXISTS player_item（进程内只执行一次） */
    bool EnsureTable();

    /**
     * 插入一条道具获得记录。
     * @param expire_time_sec  Unix 秒；<=0 表示 expire_time 列为 NULL
     * @param extra_data       JSON 字符串；空则使用 {"source":"grant_item"}
     * @param instance_id      输出自增主键 id（player_item.id）
     */
    bool Insert(uint64_t player_id, uint64_t item_id, uint32_t count, int64_t expire_time_sec,
                const std::string &extra_data, uint64_t *instance_id);

    /**
     * 在已有连接上 INSERT（用于邮件领取等事务；不单独取连接）。
     * @param conn 非空且调用方负责事务边界
     */
    bool InsertOnConnection(class Connection *conn, uint64_t player_id, uint64_t item_id,
                            uint32_t count, int64_t expire_time_sec, const std::string &extra_data,
                            uint64_t *instance_id);

private:
    PlayerItemStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
