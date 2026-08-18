#include "MetricsDbWriter.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include "ProcessMetricsSnapshot.h"

#include "EventLoop.h"

#include <mysql/mysql.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

std::mutex g_flush_mutex;

std::string SqlQuoted(const std::string &s) {
    std::string out = "'";
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '\'')
            out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string SqlDouble(double v) {
    if (v < 0.0)
        return "NULL";
    std::ostringstream os;
    os << v;
    return os.str();
}

std::string SqlInt64(int64_t v) {
    if (v < 0)
        return "NULL";
    return std::to_string(v);
}

std::string SqlUInt64(uint64_t v) {
    return std::to_string(v);
}

}  // namespace

MetricsDbWriter &MetricsDbWriter::Instance() {
    static MetricsDbWriter g;
    return g;
}

void MetricsDbWriter::StartPeriodic(EventLoop *loop, double interval_sec) {
    if (!loop || !ConnectionPool::getconnectionPool()->isInitialized())
        return;
    LOG_INFO << "MetricsDbWriter: flush to MySQL every " << interval_sec << "s";
    loop->RunEvery(interval_sec, [this]() { OnTick(); });
}

void MetricsDbWriter::OnTick() {
    std::thread([this]() {
        std::lock_guard<std::mutex> lk(g_flush_mutex);
        if (!EnsureTable())
            return;
        (void)InsertSnapshot();
    }).detach();
}

bool MetricsDbWriter::EnsureTable() {
    static bool logged_ready = false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS gamemesh_metrics ("
        "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
        "ts_unix BIGINT NOT NULL,"
        "cpu_seconds_total DOUBLE,"
        "rss_bytes BIGINT,"
        "vm_size_bytes BIGINT,"
        "open_fds INT,"
        "process_threads INT,"
        "eventloop_tick_sec DOUBLE,"
        "eventloop_tick_peak_sec DOUBLE,"
        "thread_states_json TEXT,"
        "tcp_send_queue_bytes BIGINT,"
        "tcp_recv_queue_bytes BIGINT,"
        "KEY idx_ts (ts_unix)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    const bool ok = conn->update(sql);
    if (ok && !logged_ready) {
        logged_ready = true;
        LOG_INFO << "MetricsDbWriter: table gamemesh_metrics ready";
    }
    return ok;
}

bool MetricsDbWriter::InsertSnapshot() {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;

    ProcessMetricsSnapshot m;
    if (!FillProcessMetricsSnapshot(&m))
        return false;

    const std::string thread_json =
        m.thread_states_json.empty() ? "{}" : m.thread_states_json;

    std::ostringstream sql;
    sql << "INSERT INTO gamemesh_metrics ("
           "ts_unix,cpu_seconds_total,rss_bytes,vm_size_bytes,open_fds,process_threads,"
           "eventloop_tick_sec,eventloop_tick_peak_sec,thread_states_json,"
           "tcp_send_queue_bytes,tcp_recv_queue_bytes) VALUES ("
        << m.time_unix << ","
        << SqlDouble(m.cpu_seconds_total) << ","
        << SqlInt64(m.rss_bytes) << ","
        << SqlInt64(m.vm_size_bytes) << ","
        << SqlInt64(m.open_fds) << ","
        << SqlInt64(m.threads) << ","
        << m.eventloop_tick_sec << ","
        << m.eventloop_tick_peak_sec << ","
        << SqlQuoted(thread_json) << ","
        << SqlUInt64(m.tcp_send_queue_bytes) << ","
        << SqlUInt64(m.tcp_recv_queue_bytes) << ")";
    const bool ok = conn->update(sql.str());
    if (ok) {
        LOG_INFO << "MetricsDbWriter: inserted metrics row ts=" << m.time_unix;
    } else {
        LOG_ERROR << "MetricsDbWriter: insert failed";
    }
    return ok;
}

namespace {

int64_t ParseI64(const char *s) {
    return s ? std::strtoll(s, nullptr, 10) : 0;
}

uint64_t ParseU64(const char *s) {
    return s ? std::strtoull(s, nullptr, 10) : 0;
}

double ParseD(const char *s) {
    return s ? std::strtod(s, nullptr) : 0;
}

}  // namespace

bool MetricsDbWriter::QueryHistory(int hours, int limit, MetricsHistoryQuery *out) {
    if (!out)
        return false;
    *out = MetricsHistoryQuery{};
    auto *pool = ConnectionPool::getconnectionPool();
    if (!pool || !pool->isInitialized()) {
        out->error = "mysql pool not initialized";
        return true;
    }
    out->mysql = true;
    if (!EnsureTable()) {
        out->error = "gamemesh_metrics unavailable";
        return true;
    }
    auto conn = pool->getConnection();
    if (!conn) {
        out->error = "no connection";
        return true;
    }
    MYSQL_RES *sum = conn->query("SELECT COUNT(*), MAX(ts_unix) FROM gamemesh_metrics");
    if (sum) {
        MYSQL_ROW row = mysql_fetch_row(sum);
        if (row) {
            out->total_rows = ParseU64(row[0]);
            out->last_ts_unix = ParseI64(row[1]);
        }
        mysql_free_result(sum);
    }
    if (hours < 1)
        hours = 1;
    if (hours > 168)
        hours = 168;
    if (limit < 1)
        limit = 1;
    if (limit > 2000)
        limit = 2000;
    const int64_t since =
        static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(hours) * 3600;
    std::ostringstream sql;
    sql << "SELECT ts_unix,cpu_seconds_total,rss_bytes,vm_size_bytes,open_fds,process_threads,"
           "eventloop_tick_sec,eventloop_tick_peak_sec,thread_states_json,"
           "tcp_send_queue_bytes,tcp_recv_queue_bytes FROM ("
           "SELECT ts_unix,cpu_seconds_total,rss_bytes,vm_size_bytes,open_fds,process_threads,"
           "eventloop_tick_sec,eventloop_tick_peak_sec,thread_states_json,"
           "tcp_send_queue_bytes,tcp_recv_queue_bytes FROM gamemesh_metrics WHERE ts_unix>="
        << since << " ORDER BY ts_unix DESC LIMIT " << limit
        << ") t ORDER BY ts_unix ASC";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res) {
        out->error = "query failed";
        return true;
    }
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        MetricsHistoryPoint p;
        p.ts_unix = ParseI64(row[0]);
        p.cpu_seconds_total = ParseD(row[1]);
        p.rss_bytes = ParseI64(row[2]);
        p.vm_size_bytes = ParseI64(row[3]);
        p.open_fds = static_cast<int>(ParseI64(row[4]));
        p.process_threads = static_cast<int>(ParseI64(row[5]));
        p.eventloop_tick_sec = ParseD(row[6]);
        p.eventloop_tick_peak_sec = ParseD(row[7]);
        p.thread_states_json = row[8] ? row[8] : "";
        p.tcp_send_queue_bytes = ParseU64(row[9]);
        p.tcp_recv_queue_bytes = ParseU64(row[10]);
        out->points.push_back(p);
    }
    mysql_free_result(res);
    out->ok = true;
    return true;
}
