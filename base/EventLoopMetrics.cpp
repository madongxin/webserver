#include "EventLoopMetrics.h"

#include <atomic>
#include <algorithm>

namespace {

std::atomic<double> g_last{0.0};
std::atomic<double> g_peak{0.0};
std::atomic<int> g_todo_last{0};
std::atomic<int> g_todo_recent_max{0};
std::atomic<int> g_batch_last{0};
std::atomic<double> g_batch_last_sec{0.0};
std::atomic<int> g_batch_recent_max{0};
std::atomic<int> g_batch_recent_ms_max{0};

template <typename T>
void StoreMax(std::atomic<T> *slot, T value) {
    T prev = slot->load(std::memory_order_relaxed);
    while (value > prev &&
           !slot->compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

namespace EventLoopMetrics {

void RecordTick(double seconds) {
    g_last.store(seconds, std::memory_order_relaxed);
    double prev = g_peak.load(std::memory_order_relaxed);
    while (seconds > prev &&
           !g_peak.compare_exchange_weak(prev, seconds, std::memory_order_relaxed)) {
    }
}

double LastTickSeconds() {
    return g_last.load(std::memory_order_relaxed);
}

double PeakTickSeconds() {
    return g_peak.load(std::memory_order_relaxed);
}

void RecordTodoDepth(int depth) {
    if (depth < 0)
        depth = 0;
    g_todo_last.store(depth, std::memory_order_relaxed);
    StoreMax(&g_todo_recent_max, depth);
}

void RecordBatch(int n, double seconds) {
    if (n < 0)
        n = 0;
    g_batch_last.store(n, std::memory_order_relaxed);
    g_batch_last_sec.store(seconds, std::memory_order_relaxed);
    StoreMax(&g_batch_recent_max, n);
    const int ms = static_cast<int>(seconds * 1000.0);
    if (ms > 0)
        StoreMax(&g_batch_recent_ms_max, ms);
}

int LastTodoDepth() { return g_todo_last.load(std::memory_order_relaxed); }
int LastBatchSize() { return g_batch_last.load(std::memory_order_relaxed); }
double LastBatchSeconds() { return g_batch_last_sec.load(std::memory_order_relaxed); }

int ExchangeRecentTodoMax() { return g_todo_recent_max.exchange(0, std::memory_order_relaxed); }
int ExchangeRecentBatchMax() { return g_batch_recent_max.exchange(0, std::memory_order_relaxed); }
int ExchangeRecentBatchMsMax() {
    return g_batch_recent_ms_max.exchange(0, std::memory_order_relaxed);
}

}  // namespace EventLoopMetrics
