#include "MetricsDbWriter.h"

#include "ConnectionPool.h"
#include "Logging.h"
#include "ProcessMetricsSnapshot.h"

#include "EventLoop.h"

#include <chrono>
#include <cstdio>
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
