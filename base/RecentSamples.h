#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

/** 最近 256 个样本。没有样本时 Percentile 返回 false，监控侧显示无数据。 */
class RecentSamples {
public:
    void Add(int64_t v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.size() < 256)
            buf_.push_back(v);
        else
            buf_[static_cast<size_t>(n_ % 256)] = v;
        ++n_;
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lk(mu_);
        return buf_.empty();
    }

    bool Percentile(double q, int64_t *out) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.empty() || !out)
            return false;
        std::vector<int64_t> sorted = buf_;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
        if (idx >= sorted.size())
            idx = sorted.size() - 1;
        *out = sorted[idx];
        return true;
    }

private:
    mutable std::mutex mu_;
    std::vector<int64_t> buf_;
    uint64_t n_ = 0;
};

inline RecentSamples &ReadWakeBytesSamples() {
    static RecentSamples samples;
    return samples;
}

inline RecentSamples &OnMessageUsSamples() {
    static RecentSamples samples;
    return samples;
}

inline RecentSamples &BatchFramesSamples() {
    static RecentSamples samples;
    return samples;
}
