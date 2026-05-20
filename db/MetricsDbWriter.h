#pragma once

class EventLoop;

class MetricsDbWriter {
public:
    static MetricsDbWriter &Instance();

    void StartPeriodic(EventLoop *loop, double interval_sec);

private:
    MetricsDbWriter() = default;
    void OnTick();
    bool EnsureTable();
    bool InsertSnapshot();
};
