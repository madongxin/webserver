#include "LogicMetrics.h"

#include <atomic>

namespace {

std::atomic<double> g_last{0.0};
std::atomic<double> g_peak{0.0};

}  // namespace

namespace LogicMetrics {

void RecordHandle(double seconds) {
    g_last.store(seconds, std::memory_order_relaxed);
    double prev = g_peak.load(std::memory_order_relaxed);
    while (seconds > prev &&
           !g_peak.compare_exchange_weak(prev, seconds, std::memory_order_relaxed)) {
    }
}

double LastHandleSeconds() {
    return g_last.load(std::memory_order_relaxed);
}

double PeakHandleSeconds() {
    return g_peak.load(std::memory_order_relaxed);
}

}  // namespace LogicMetrics
