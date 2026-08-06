/**
 * @file PlayerItemStore.cpp
 * @brief player_item 表的 DDL 与 INSERT 实现
 */

#include "PlayerItemStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;

/** 转义 SQL 字符串中的 \ 和 '，防止拼接注入 */
std::string SqlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '\'')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

PlayerItemStore &PlayerItemStore::Instance() {
    static PlayerItemStore g;
    return g;
}

bool PlayerItemStore::EnsureTable() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (table_ready_)
        return true;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        available_ = false;
        return false;
    }
    available_ = true;

    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;

    // 每条发放记录一行；id 为道具实例唯一 ID，非配置表 item_id
    const char *sql =
        "CREATE TABLE IF NOT EXISTS player_item ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "player_id BIGINT NOT NULL,"
        "item_id BIGINT NOT NULL,"
        "count INT NOT NULL DEFAULT 1,"
        "create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "expire_time DATETIME NULL,"
        "extra_data JSON,"
        "KEY idx_player_item (player_id, item_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!conn->update(sql))
        return false;
    table_ready_ = true;
    LOG_INFO << "PlayerItemStore: table player_item ready";
    return true;
}

bool PlayerItemStore::Insert(uint64_t player_id, uint64_t item_id, uint32_t count,
                             int64_t expire_time_sec, const std::string &extra_data,
                             uint64_t *instance_id) {
    if (!instance_id || count == 0)
        return false;
    if (!EnsureTable())
        return false;

    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;

    std::string extra = extra_data.empty() ? "{\"source\":\"grant_item\"}" : extra_data;

    std::ostringstream sql;
    sql << "INSERT INTO player_item (player_id,item_id,count,extra_data";
    if (expire_time_sec > 0)
        sql << ",expire_time";
    sql << ") VALUES (" << player_id << "," << item_id << "," << count << ",'"
        << SqlEscape(extra) << "'";
    if (expire_time_sec > 0)
        sql << ",FROM_UNIXTIME(" << expire_time_sec << ")";
    sql << ")";

    if (!conn->update(sql.str()))
        return false;

    // 取 AUTO_INCREMENT，作为 GrantItemRsp.instance_id（队列入库时客户端多为 0）
    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        return false;
    }
    *instance_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    return *instance_id > 0;
}

bool PlayerItemStore::InsertOnConnection(Connection *conn, uint64_t player_id, uint64_t item_id,
                                         uint32_t count, int64_t expire_time_sec,
                                         const std::string &extra_data, uint64_t *instance_id) {
    if (!conn || !instance_id || count == 0)
        return false;
    if (!EnsureTable())
        return false;

    std::string extra = extra_data.empty() ? "{\"source\":\"mail_claim\"}" : extra_data;
    std::ostringstream sql;
    sql << "INSERT INTO player_item (player_id,item_id,count,extra_data";
    if (expire_time_sec > 0)
        sql << ",expire_time";
    sql << ") VALUES (" << player_id << "," << item_id << "," << count << ",'" << SqlEscape(extra)
        << "'";
    if (expire_time_sec > 0)
        sql << ",FROM_UNIXTIME(" << expire_time_sec << ")";
    sql << ")";
    if (!conn->update(sql.str()))
        return false;

    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        return false;
    }
    *instance_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    return *instance_id > 0;
}
