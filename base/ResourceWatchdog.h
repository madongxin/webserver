#pragma once

#include <json/json.h>
#include <mutex>
#include <vector>

class EventLoop;

// 定时采样 VmRSS / fd / TCP 队列，供 GM 命令做「缓增、暴涨、fd 爬升、网络队列爬升」等启发式判断。
class ResourceWatchdog {
public:
    struct Sample {
        int64_t time_unix = 0;
        int64_t rss_kb = 0;
        int64_t vm_size_kb = 0;
        int fds = 0;
        uint64_t tcp_send_q = 0;
        uint64_t tcp_recv_q = 0;
    };

    static ResourceWatchdog &Instance();

    void StartPeriodic(EventLoop *loop, double interval_sec);

    void PushSample();

    Json::Value LastSampleJson() const;
    Json::Value AnalyzeTrends() const;
    Json::Value HistoryJson(int max_points) const;

private:
    ResourceWatchdog() = default;

    mutable std::mutex mutex_;
    std::vector<Sample> samples_;
    static constexpr size_t kMaxSamples = 512;
};
