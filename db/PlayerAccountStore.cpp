#include "PlayerAccountStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"

#include <mysql/mysql.h>

#include <cstdlib>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;

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

PlayerAccountStore &PlayerAccountStore::Instance() {
    static PlayerAccountStore g;
    return g;
}

bool PlayerAccountStore::EnsureTable() {
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
    const char *sql =
        "CREATE TABLE IF NOT EXISTS player_account ("
        "player_id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "device_id VARCHAR(128) NOT NULL DEFAULT '',"
        "display_name VARCHAR(64) NOT NULL DEFAULT '',"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "KEY idx_player_account_device (device_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!conn->update(sql))
        return false;
    table_ready_ = true;
    LOG_INFO << "PlayerAccountStore: table player_account ready";
    return true;
}

bool PlayerAccountStore::Register(const std::string &device_id, const std::string &display_name,
                                  uint64_t *player_id, std::string *err) {
    if (!player_id) {
        if (err)
            *err = "bad arg";
        return false;
    }
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "no connection";
        return false;
    }
    std::string name = display_name.empty() ? "player" : display_name;
    if (name.size() > 64)
        name.resize(64);
    std::ostringstream sql;
    sql << "INSERT INTO player_account (device_id,display_name) VALUES ('" << SqlEscape(device_id)
        << "','" << SqlEscape(name) << "')";
    if (!conn->update(sql.str())) {
        if (err)
            *err = "insert failed";
        return false;
    }
    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res) {
        if (err)
            *err = "no player_id";
        return false;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        if (err)
            *err = "no player_id";
        return false;
    }
    *player_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    if (*player_id == 0) {
        if (err)
            *err = "invalid player_id";
        return false;
    }
    return true;
}

bool PlayerAccountStore::Exists(uint64_t player_id) {
    if (player_id == 0 || !EnsureTable())
        return false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT 1 FROM player_account WHERE player_id=" << player_id << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = row != nullptr;
    mysql_free_result(res);
    return ok;
}
