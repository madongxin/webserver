#pragma once

#include <cstdint>
#include <string>
#include <vector>

class EventLoop;

struct MetricsHistoryPoint {
    int64_t ts_unix = 0;
    double cpu_seconds_total = 0;
    int64_t rss_bytes = 0;
    int64_t vm_size_bytes = 0;
    int open_fds = 0;
    int process_threads = 0;
    double eventloop_tick_sec = 0;
    double eventloop_tick_peak_sec = 0;
    std::string thread_states_json;
    uint64_t tcp_send_queue_bytes = 0;
    uint64_t tcp_recv_queue_bytes = 0;
};

struct MetricsHistoryQuery {
    bool ok = false;
    bool mysql = false;
    std::string error;
    uint64_t total_rows = 0;
    int64_t last_ts_unix = 0;
    std::vector<MetricsHistoryPoint> points;
};

class MetricsDbWriter {
public:
    static MetricsDbWriter &Instance();

    void StartPeriodic(EventLoop *loop, double interval_sec);
    bool QueryHistory(int hours, int limit, MetricsHistoryQuery *out);

private:
    MetricsDbWriter() = default;
    void OnTick();
    bool EnsureTable();
    bool InsertSnapshot();
};
