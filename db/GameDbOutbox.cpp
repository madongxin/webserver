#include "GameDbOutbox.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"

#include <mysql/mysql.h>

#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;

std::string SqlEscape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'' || c == '\\')
            o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

}  // namespace

GameDbOutbox &GameDbOutbox::Instance() {
    static GameDbOutbox g;
    return g;
}

bool GameDbOutbox::EnsureTable() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (table_ready_)
        return true;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        available_ = false;
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS gamedb_outbox ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "event_type VARCHAR(64) NOT NULL,"
        "aggregate_type VARCHAR(64) NOT NULL,"
        "aggregate_id VARCHAR(128) NOT NULL,"
        "idempotency_key VARCHAR(128) NOT NULL,"
        "payload TEXT NOT NULL,"
        "created_at BIGINT NOT NULL,"
        "published_at BIGINT NULL,"
        "UNIQUE KEY uk_gamedb_outbox_idem (idempotency_key),"
        "KEY idx_gamedb_outbox_unpub (published_at, id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!conn->update(sql)) {
        LOG_WARN << "GameDbOutbox: CREATE TABLE failed";
        return false;
    }
    table_ready_ = true;
    available_ = true;
    LOG_INFO << "GameDbOutbox: table ready";
    return true;
}

bool GameDbOutbox::InsertOnConnection(Connection *conn, const std::string &event_type,
                                      const std::string &aggregate_type,
                                      const std::string &aggregate_id,
                                      const std::string &idempotency_key, const std::string &payload,
                                      int64_t created_at) {
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "INSERT INTO gamedb_outbox (event_type,aggregate_type,aggregate_id,idempotency_key,"
        << "payload,created_at,published_at) VALUES ('" << SqlEscape(event_type) << "','"
        << SqlEscape(aggregate_type) << "','" << SqlEscape(aggregate_id) << "','"
        << SqlEscape(idempotency_key) << "','" << SqlEscape(payload) << "'," << created_at
        << ",NULL)";
    return conn->update(sql.str());
}

bool GameDbOutbox::FetchUnpublished(int limit, std::vector<GameDbOutboxRow> *out) {
    if (!out || !EnsureTable())
        return false;
    out->clear();
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "SELECT id,event_type,aggregate_type,aggregate_id,idempotency_key,payload,created_at,"
        << "IFNULL(published_at,0) FROM gamedb_outbox WHERE published_at IS NULL ORDER BY id "
        << "LIMIT " << (limit > 0 ? limit : 100);
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        GameDbOutboxRow r;
        r.id = row[0] ? std::strtoull(row[0], nullptr, 10) : 0;
        r.event_type = row[1] ? row[1] : "";
        r.aggregate_type = row[2] ? row[2] : "";
        r.aggregate_id = row[3] ? row[3] : "";
        r.idempotency_key = row[4] ? row[4] : "";
        r.payload = row[5] ? row[5] : "";
        r.created_at = row[6] ? std::strtoll(row[6], nullptr, 10) : 0;
        r.published_at = row[7] ? std::strtoll(row[7], nullptr, 10) : 0;
        out->push_back(std::move(r));
    }
    mysql_free_result(res);
    return true;
}

bool GameDbOutbox::MarkPublished(uint64_t id, int64_t published_at) {
    if (!EnsureTable())
        return false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    std::ostringstream sql;
    sql << "UPDATE gamedb_outbox SET published_at=" << published_at << " WHERE id=" << id
        << " AND published_at IS NULL";
    return conn->update(sql.str());
}

int GameDbOutbox::CountByIdempotency(const std::string &idempotency_key) {
    if (!EnsureTable())
        return -1;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return -1;
    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM gamedb_outbox WHERE idempotency_key='"
        << SqlEscape(idempotency_key) << "'";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    const int n = row && row[0] ? std::atoi(row[0]) : 0;
    mysql_free_result(res);
    return n;
}
