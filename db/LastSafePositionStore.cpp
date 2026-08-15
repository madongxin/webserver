#include "LastSafePositionStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "FormalMode.h"
#include "Logging.h"

#include <mysql/mysql.h>

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <sstream>

namespace {

std::mutex g_mu;

const char kDdl[] =
    "CREATE TABLE IF NOT EXISTS player_last_safe_position ("
    "player_id BIGINT NOT NULL PRIMARY KEY,"
    "realm_id INT NOT NULL DEFAULT 1,"
    "map_template_id BIGINT NOT NULL,"
    "last_safe_x FLOAT NOT NULL,"
    "last_safe_y FLOAT NOT NULL,"
    "last_safe_z FLOAT NOT NULL,"
    "last_safe_yaw FLOAT NOT NULL,"
    "position_version BIGINT NOT NULL DEFAULT 1,"
    "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

const char *kCols[] = {"player_id",    "realm_id",     "map_template_id", "last_safe_x",
                       "last_safe_y",  "last_safe_z",  "last_safe_yaw",   "position_version"};

bool TableExists(Connection *conn) {
    MYSQL_RES *res = conn->query("SHOW TABLES LIKE 'player_last_safe_position'");
    if (!res)
        return false;
    const bool ok = mysql_fetch_row(res) != nullptr;
    mysql_free_result(res);
    return ok;
}

bool ColumnsOk(Connection *conn) {
    MYSQL_RES *res = conn->query("SHOW COLUMNS FROM player_last_safe_position");
    if (!res)
        return false;
    std::set<std::string> cols;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0])
            cols.insert(row[0]);
    }
    mysql_free_result(res);
    for (const char *c : kCols) {
        if (!cols.count(c))
            return false;
    }
    return true;
}

}  // namespace

LastSafePositionStore &LastSafePositionStore::Instance() {
    static LastSafePositionStore s;
    return s;
}

bool LastSafePositionStore::ValidateFinite(const LastSafePositionRow &row, std::string *err_code,
                                           std::string *err) {
    if (row.player_id == 0 || row.map_template_id == 0) {
        if (err_code)
            *err_code = "ERR_INVALID_ARGUMENT";
        if (err)
            *err = "player_id/map_template_id required";
        return false;
    }
    if (!std::isfinite(row.x) || !std::isfinite(row.y) || !std::isfinite(row.z) ||
        !std::isfinite(row.yaw)) {
        if (err_code)
            *err_code = "ERR_INVALID_POSITION";
        if (err)
            *err = "non-finite last_safe coordinates";
        return false;
    }
    return true;
}

bool LastSafePositionStore::EnsureTable() {
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
    if (BootstrapDdlEnabled()) {
        if (!conn->update(kDdl))
            return false;
    }
    if (!TableExists(conn.get())) {
        available_ = false;
        LOG_ERROR << "LastSafePositionStore: table missing; run ./scripts/migrate_db.sh";
        return false;
    }
    if (!ColumnsOk(conn.get())) {
        available_ = false;
        LOG_ERROR << "LastSafePositionStore: columns incomplete; run migrate_db.sh";
        return false;
    }
    table_ready_ = true;
    return true;
}

bool LastSafePositionStore::Load(uint64_t player_id, LastSafePositionRow *out, std::string *err) {
    if (!out || player_id == 0) {
        if (err)
            *err = "bad arg";
        return false;
    }
    *out = LastSafePositionRow{};
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "pool exhausted";
        return false;
    }
    std::ostringstream sql;
    sql << "SELECT player_id,realm_id,map_template_id,last_safe_x,last_safe_y,last_safe_z,"
           "last_safe_yaw,position_version FROM player_last_safe_position WHERE player_id="
        << player_id << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res) {
        if (err)
            *err = "query failed";
        return false;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        out->exists = false;
        return true;
    }
    out->exists = true;
    out->player_id = static_cast<uint64_t>(std::strtoull(row[0] ? row[0] : "0", nullptr, 10));
    out->realm_id = static_cast<uint32_t>(std::strtoul(row[1] ? row[1] : "1", nullptr, 10));
    out->map_template_id =
        static_cast<uint64_t>(std::strtoull(row[2] ? row[2] : "0", nullptr, 10));
    out->x = row[3] ? std::strtof(row[3], nullptr) : 0.f;
    out->y = row[4] ? std::strtof(row[4], nullptr) : 0.f;
    out->z = row[5] ? std::strtof(row[5], nullptr) : 0.f;
    out->yaw = row[6] ? std::strtof(row[6], nullptr) : 0.f;
    out->position_version =
        static_cast<uint64_t>(std::strtoull(row[7] ? row[7] : "0", nullptr, 10));
    mysql_free_result(res);
    return true;
}

bool LastSafePositionStore::Save(const LastSafePositionRow &row, uint64_t expected_version,
                                 uint64_t *out_version, bool *skipped, std::string *err,
                                 std::string *err_code) {
    if (skipped)
        *skipped = false;
    std::string code, msg;
    if (!ValidateFinite(row, &code, &msg)) {
        if (skipped)
            *skipped = true;
        if (err_code)
            *err_code = code;
        if (err)
            *err = msg;
        return false;
    }
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        if (err_code)
            *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "pool exhausted";
        if (err_code)
            *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
        return false;
    }
    uint64_t cur_ver = 0;
    bool exists = false;
    {
        std::ostringstream q;
        q << "SELECT position_version FROM player_last_safe_position WHERE player_id="
          << row.player_id << " LIMIT 1";
        MYSQL_RES *res = conn->query(q.str());
        if (!res) {
            if (err)
                *err = "version query failed";
            if (err_code)
                *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
            return false;
        }
        MYSQL_ROW r = mysql_fetch_row(res);
        if (r) {
            exists = true;
            cur_ver = static_cast<uint64_t>(std::strtoull(r[0] ? r[0] : "0", nullptr, 10));
        }
        mysql_free_result(res);
    }
    if (exists && expected_version != 0 && cur_ver != expected_version) {
        if (err_code)
            *err_code = "ERR_STALE_SEQ";
        if (err)
            *err = "position_version mismatch";
        return false;
    }
    const uint64_t next_ver = exists ? cur_ver + 1 : 1;
    std::ostringstream sql;
    sql.setf(std::ios::fixed);
    sql.precision(6);
    sql << "INSERT INTO player_last_safe_position (player_id,realm_id,map_template_id,"
           "last_safe_x,last_safe_y,last_safe_z,last_safe_yaw,position_version) VALUES ("
        << row.player_id << "," << row.realm_id << "," << row.map_template_id << "," << row.x << ","
        << row.y << "," << row.z << "," << row.yaw << "," << next_ver
        << ") ON DUPLICATE KEY UPDATE realm_id=VALUES(realm_id),"
           "map_template_id=VALUES(map_template_id),last_safe_x=VALUES(last_safe_x),"
           "last_safe_y=VALUES(last_safe_y),last_safe_z=VALUES(last_safe_z),"
           "last_safe_yaw=VALUES(last_safe_yaw),position_version=VALUES(position_version)";
    if (!conn->update(sql.str())) {
        if (err)
            *err = "save failed";
        if (err_code)
            *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
        return false;
    }
    if (out_version)
        *out_version = next_ver;
    return true;
}
