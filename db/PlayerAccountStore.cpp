#include "PlayerAccountStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include "PlayerProfileStore.h"

#include <mysql/mysql.h>

#include <chrono>
#include <cstdio>
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
        "password_hash VARCHAR(128) NOT NULL DEFAULT '',"
        "password_salt VARCHAR(64) NOT NULL DEFAULT '',"
        "password_iters INT NOT NULL DEFAULT 0,"
        "banned TINYINT NOT NULL DEFAULT 0,"
        "idempotency_key VARCHAR(128) NOT NULL DEFAULT '',"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "KEY idx_player_account_device (device_id),"
        "UNIQUE KEY uk_player_account_idem (idempotency_key)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!conn->update(sql))
        return false;
    // 兼容旧表：尝试加列（已存在则忽略失败）
    conn->update("ALTER TABLE player_account ADD COLUMN password_hash VARCHAR(128) NOT NULL DEFAULT ''");
    conn->update("ALTER TABLE player_account ADD COLUMN password_salt VARCHAR(64) NOT NULL DEFAULT ''");
    conn->update("ALTER TABLE player_account ADD COLUMN password_iters INT NOT NULL DEFAULT 0");
    conn->update("ALTER TABLE player_account ADD COLUMN banned TINYINT NOT NULL DEFAULT 0");
    conn->update(
        "ALTER TABLE player_account ADD COLUMN idempotency_key VARCHAR(128) NOT NULL DEFAULT ''");
    conn->update(
        "ALTER TABLE player_account ADD UNIQUE KEY uk_player_account_idem (idempotency_key)");
    table_ready_ = true;
    LOG_INFO << "PlayerAccountStore: table player_account ready";
    return true;
}

bool PlayerAccountStore::Register(const std::string &device_id, const std::string &display_name,
                                  uint64_t *player_id, std::string *err) {
    return RegisterWithPassword(device_id, display_name, "", "", 0, player_id, err);
}

bool PlayerAccountStore::FindByIdempotencyKey(const std::string &idempotency_key,
                                              uint64_t *player_id) {
    if (!player_id || idempotency_key.empty() || !EnsureTable())
        return false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT player_id FROM player_account WHERE idempotency_key='"
        << SqlEscape(idempotency_key) << "' LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        return false;
    }
    *player_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    return *player_id != 0;
}

bool PlayerAccountStore::RegisterWithPasswordIdempotent(
    const std::string &device_id, const std::string &display_name, const std::string &password_hash,
    const std::string &password_salt, int password_iters, const std::string &idempotency_key,
    uint64_t *player_id, std::string *err, bool *replayed) {
    if (replayed)
        *replayed = false;
    if (idempotency_key.empty())
        return RegisterWithPassword(device_id, display_name, password_hash, password_salt,
                                    password_iters, player_id, err);
    if (!player_id) {
        if (err)
            *err = "bad arg";
        return false;
    }
    if (FindByIdempotencyKey(idempotency_key, player_id)) {
        if (replayed)
            *replayed = true;
        AccountAuthRow row;
        std::string pname = display_name;
        if (LoadAuth(*player_id, &row) && !row.display_name.empty())
            pname = row.display_name;
        std::string perr;
        if (!PlayerProfileStore::Instance().EnsureDefault(*player_id, pname, &perr)) {
            if (err)
                *err = perr.empty() ? "profile backfill failed" : perr;
            return false;
        }
        return true;
    }
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        return false;
    }
    if (!PlayerProfileStore::Instance().EnsureTable()) {
        if (err)
            *err = "profile table unavailable";
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
    if (!conn->begin()) {
        if (err)
            *err = "begin failed";
        return false;
    }
    std::ostringstream sql;
    sql << "INSERT INTO player_account (device_id,display_name,password_hash,password_salt,"
           "password_iters,idempotency_key) VALUES ('"
        << SqlEscape(device_id) << "','" << SqlEscape(name) << "','" << SqlEscape(password_hash)
        << "','" << SqlEscape(password_salt) << "'," << password_iters << ",'"
        << SqlEscape(idempotency_key) << "')";
    if (!conn->update(sql.str())) {
        conn->rollback();
        // 并发唯一冲突：回读首次结果并补 Profile
        if (FindByIdempotencyKey(idempotency_key, player_id)) {
            if (replayed)
                *replayed = true;
            std::string perr;
            if (!PlayerProfileStore::Instance().EnsureDefault(*player_id, name, &perr)) {
                if (err)
                    *err = perr.empty() ? "profile backfill failed" : perr;
                return false;
            }
            return true;
        }
        if (err)
            *err = "insert failed";
        return false;
    }
    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res) {
        conn->rollback();
        if (err)
            *err = "no player_id";
        return false;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        conn->rollback();
        if (err)
            *err = "no player_id";
        return false;
    }
    *player_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    if (*player_id == 0) {
        conn->rollback();
        if (err)
            *err = "invalid player_id";
        return false;
    }
    std::string perr;
    if (!PlayerProfileStore::Instance().InsertDefaultOnConnection(conn.get(), *player_id, name,
                                                                  &perr)) {
        conn->rollback();
        if (err)
            *err = perr.empty() ? "profile insert failed" : perr;
        return false;
    }
    if (!conn->commit()) {
        conn->rollback();
        if (err)
            *err = "commit failed";
        return false;
    }
    return true;
}

bool PlayerAccountStore::RegisterWithPassword(const std::string &device_id,
                                              const std::string &display_name,
                                              const std::string &password_hash,
                                              const std::string &password_salt, int password_iters,
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
    if (!PlayerProfileStore::Instance().EnsureTable()) {
        if (err)
            *err = "profile table unavailable";
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
    // 无幂等键时用空串占位；uk 允许多个 '' 在 MySQL 中对 UNIQUE 空串特殊——用随机占位避免冲突
    // 非幂等路径：idempotency_key = 'na:' + player 将在插入后不可用，改用时间戳+随机
    char na[64];
    std::snprintf(na, sizeof(na), "na:%lld:%u",
                  static_cast<long long>(
                      std::chrono::system_clock::now().time_since_epoch().count()),
                  static_cast<unsigned>(std::rand()));
    if (!conn->begin()) {
        if (err)
            *err = "begin failed";
        return false;
    }
    std::ostringstream sql;
    sql << "INSERT INTO player_account (device_id,display_name,password_hash,password_salt,"
           "password_iters,idempotency_key) VALUES ('"
        << SqlEscape(device_id) << "','" << SqlEscape(name) << "','" << SqlEscape(password_hash)
        << "','" << SqlEscape(password_salt) << "'," << password_iters << ",'" << SqlEscape(na)
        << "')";
    if (!conn->update(sql.str())) {
        conn->rollback();
        if (err)
            *err = "insert failed";
        return false;
    }
    MYSQL_RES *res = conn->query("SELECT LAST_INSERT_ID() AS id");
    if (!res) {
        conn->rollback();
        if (err)
            *err = "no player_id";
        return false;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        conn->rollback();
        if (err)
            *err = "no player_id";
        return false;
    }
    *player_id = static_cast<uint64_t>(std::strtoull(row[0], nullptr, 10));
    mysql_free_result(res);
    if (*player_id == 0) {
        conn->rollback();
        if (err)
            *err = "invalid player_id";
        return false;
    }
    std::string perr;
    if (!PlayerProfileStore::Instance().InsertDefaultOnConnection(conn.get(), *player_id, name,
                                                                  &perr)) {
        conn->rollback();
        if (err)
            *err = perr.empty() ? "profile insert failed" : perr;
        return false;
    }
    if (!conn->commit()) {
        conn->rollback();
        if (err)
            *err = "commit failed";
        return false;
    }
    return true;
}

bool PlayerAccountStore::Exists(uint64_t player_id) {
    AccountAuthRow row;
    return LoadAuth(player_id, &row) && row.exists;
}

bool PlayerAccountStore::LoadAuth(uint64_t player_id, AccountAuthRow *out) {
    if (!out || player_id == 0 || !EnsureTable())
        return false;
    *out = AccountAuthRow{};
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT player_id,IFNULL(password_hash,''),IFNULL(password_salt,''),"
           "IFNULL(password_iters,0),IFNULL(banned,0),IFNULL(display_name,'') FROM player_account WHERE player_id="
        << player_id << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        out->exists = false;
        return true;
    }
    out->exists = true;
    out->player_id = static_cast<uint64_t>(std::strtoull(row[0] ? row[0] : "0", nullptr, 10));
    out->account_id = out->player_id;
    out->password_hash = row[1] ? row[1] : "";
    out->password_salt = row[2] ? row[2] : "";
    out->password_iters = row[3] ? std::atoi(row[3]) : 0;
    out->banned = row[4] && std::atoi(row[4]) != 0;
    out->display_name = row[5] ? row[5] : "";
    out->has_password = !out->password_hash.empty() && !out->password_salt.empty() &&
                        out->password_iters > 0;
    mysql_free_result(res);
    return true;
}
