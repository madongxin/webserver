#include "ResourceWatchdog.h"

#include <algorithm>
#include <ctime>

#include "EventLoop.h"
#include "ProcSelfStats.h"

void ResourceWatchdog::StartPeriodic(EventLoop *loop, double interval_sec) {
    if (!loop || interval_sec <= 0.0)
        return;
    loop->RunEvery(interval_sec, [this]() { PushSample(); });
}

void ResourceWatchdog::PushSample() {
    ProcResourceSnapshot snap{};
    if (!FillProcResourceSnapshot(&snap, 0))
        return;
    Sample s;
    s.time_unix = static_cast<int64_t>(std::time(nullptr));
    s.rss_kb = snap.vm_rss_kb;
    s.vm_size_kb = snap.vm_size_kb;
    s.fds = snap.open_fds;
    s.tcp_send_q = snap.tcp_send_queue_bytes;
    s.tcp_recv_q = snap.tcp_recv_queue_bytes;

    std::lock_guard<std::mutex> lk(mutex_);
    samples_.push_back(s);
    if (samples_.size() > kMaxSamples)
        samples_.erase(samples_.begin(), samples_.begin() + (samples_.size() - kMaxSamples));
}

ResourceWatchdog &ResourceWatchdog::Instance() {
    static ResourceWatchdog w;
    return w;
}

namespace {

double MeanRss(const std::vector<ResourceWatchdog::Sample> &v, size_t from, size_t to) {
    if (from >= to || to > v.size())
        return 0;
    int64_t sum = 0;
    for (size_t i = from; i < to; ++i)
        sum += v[i].rss_kb;
    return static_cast<double>(sum) / static_cast<double>(to - from);
}

double MedianRss(const std::vector<ResourceWatchdog::Sample> &v, size_t from, size_t to) {
    if (from >= to || to > v.size())
        return 0;
    std::vector<int64_t> xs;
    xs.reserve(to - from);
    for (size_t i = from; i < to; ++i)
        xs.push_back(v[i].rss_kb);
    const size_t mid = xs.size() / 2;
    std::nth_element(xs.begin(), xs.begin() + mid, xs.end());
    return static_cast<double>(xs[mid]);
}

}  // namespace

Json::Value ResourceWatchdog::LastSampleJson() const {
    std::lock_guard<std::mutex> lk(mutex_);
    Json::Value j;
    if (samples_.empty())
        return j;
    const Sample &s = samples_.back();
    j["time_unix"] = Json::Int64(s.time_unix);
    j["vm_rss_kb"] = Json::Int64(s.rss_kb);
    j["vm_size_kb"] = Json::Int64(s.vm_size_kb);
    j["open_fds"] = s.fds;
    j["tcp_send_queue_bytes"] = Json::Value::UInt64(s.tcp_send_q);
    j["tcp_recv_queue_bytes"] = Json::Value::UInt64(s.tcp_recv_q);
    return j;
}

Json::Value ResourceWatchdog::HistoryJson(int max_points) const {
    std::lock_guard<std::mutex> lk(mutex_);
    Json::Value arr(Json::arrayValue);
    if (max_points <= 0 || samples_.empty())
        return arr;
    const size_t n = samples_.size();
    const size_t from = (n > static_cast<size_t>(max_points)) ? (n - static_cast<size_t>(max_points)) : 0;
    for (size_t i = from; i < n; ++i) {
        Json::Value p;
        p["t"] = Json::Int64(samples_[i].time_unix);
        p["rss_kb"] = Json::Int64(samples_[i].rss_kb);
        p["fds"] = samples_[i].fds;
        p["tcp_send_q"] = Json::Value::UInt64(samples_[i].tcp_send_q);
        p["tcp_recv_q"] = Json::Value::UInt64(samples_[i].tcp_recv_q);
        arr.append(p);
    }
    return arr;
}

Json::Value ResourceWatchdog::AnalyzeTrends() const {
    std::lock_guard<std::mutex> lk(mutex_);
    Json::Value root;
    Json::Value alerts(Json::arrayValue);
    const size_t n = samples_.size();
    root["sample_count"] = static_cast<Json::UInt>(n);
    if (n < 6) {
        root["note"] = "样本不足（定时器默认 5s 一次，多等一会或先执行 GM: watch）";
        root["alerts"] = alerts;
        return root;
    }

    const int64_t t0 = samples_.front().time_unix;
    const int64_t t1 = samples_.back().time_unix;
    const double span_sec = static_cast<double>(t1 - t0);
    root["window_sec"] = span_sec;

    const size_t q = std::max<size_t>(3, n / 4);
    const double early = MeanRss(samples_, 0, q);
    const double late = MeanRss(samples_, n - q, n);
    const double rss_slope_kb_per_min =
        (span_sec > 1.0) ? (late - early) * 60.0 / span_sec : 0.0;
    root["early_avg_rss_kb"] = early;
    root["late_avg_rss_kb"] = late;
    root["rss_slope_kb_per_min"] = rss_slope_kb_per_min;

    // 缓慢常驻增长：斜率 + 绝对增量（阈值可按机器调）
    if (span_sec >= 60.0 && rss_slope_kb_per_min > 256.0 && (late - early) > 4096.0) {
        alerts.append(
            "VmRSS 呈持续抬升（早期均值→晚期均值），疑似缓慢泄漏或缓存未释放；建议对照 pmap/ASan。");
        root["hint_slow_rss_growth"] = true;
    } else {
        root["hint_slow_rss_growth"] = false;
    }

    const double med = MedianRss(samples_, (n > 20 ? n - 20 : 0), n - 1);
    const int64_t last_rss = samples_.back().rss_kb;
    const double spike_vs_med = (med > 0.1) ? (static_cast<double>(last_rss) / med) : 1.0;
    root["median_rss_kb_window"] = med;
    root["spike_ratio_vs_median"] = spike_vs_med;
    if (last_rss > static_cast<int64_t>(med * 1.35) && static_cast<double>(last_rss - med) > 5120.0) {
        alerts.append("VmRSS 相对近期中位数明显跳升，疑似短时暴涨（尖峰或大分配）。");
        root["hint_rss_spike"] = true;
    } else {
        root["hint_rss_spike"] = false;
    }

    bool fd_non_decreasing = true;
    const size_t win = std::min<size_t>(12, n);
    for (size_t i = n - win; i + 1 < n; ++i) {
        if (samples_[i + 1].fds < samples_[i].fds)
            fd_non_decreasing = false;
    }
    root["fd_delta_in_window"] = samples_.back().fds - samples_[n - win].fds;
    root["fd_monotonic_recent"] = fd_non_decreasing;
    if (fd_non_decreasing && (samples_.back().fds - samples_[n - win].fds) >= 8) {
        alerts.append("打开 fd 数在近期窗口内持续上升，疑似 fd 泄漏或未 close。");
        root["hint_fd_leak"] = true;
    } else {
        root["hint_fd_leak"] = false;
    }

    const uint64_t q0 = samples_[n - win].tcp_send_q;
    const uint64_t q1 = samples_.back().tcp_send_q;
    root["tcp_send_q_first"] = Json::Value::UInt64(q0);
    root["tcp_send_q_last"] = Json::Value::UInt64(q1);
    const bool send_grows = (q1 > q0 + 65536u) && (q0 > 0 ? (q1 > q0 * 2u) : (q1 > 256 * 1024u));
    if (send_grows) {
        alerts.append(
            "内核 TCP 发送队列（SIOCOUTQ 汇总）持续偏高或上升，对端慢/未读或本机 send 过快；可对照 ss -tn。");
        root["hint_tcp_send_queue"] = true;
    } else {
        root["hint_tcp_send_queue"] = false;
    }

    const uint64_t r0 = samples_[n - win].tcp_recv_q;
    const uint64_t r1 = samples_.back().tcp_recv_q;
    root["tcp_recv_q_first"] = Json::Value::UInt64(r0);
    root["tcp_recv_q_last"] = Json::Value::UInt64(r1);
    if (r1 > r0 + 256 * 1024u && r1 > 512 * 1024u) {
        alerts.append("内核 TCP 接收队列（SIOCINQ 汇总）偏高，应用层读慢或积压。");
        root["hint_tcp_recv_queue"] = true;
    } else {
        root["hint_tcp_recv_queue"] = false;
    }

    root["alerts"] = alerts;
    root["compare_shell"] =
        "top -p $(pidof webserver); pmap -x $(pidof webserver) | tail; "
        "grep -E 'VmRSS|VmSize|Threads' /proc/$(pidof webserver)/status; ls /proc/$(pidof webserver)/fd | wc -l";
    return root;
}
