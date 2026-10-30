#include "PasswordExec.h"

PasswordExec &PasswordExec::Instance() {
    static PasswordExec exec;
    return exec;
}

PasswordExec::PasswordExec() {
    workers_.reserve(2);
    for (int i = 0; i < 2; ++i)
        workers_.emplace_back([this]() { Loop(); });
}

PasswordExec::~PasswordExec() {
    Stop();
}

bool PasswordExec::Post(std::function<void()> fn) {
    if (!fn)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (stopped_ || q_.size() >= 4096)
        return false;
    q_.push_back(std::move(fn));
    cv_.notify_one();
    return true;
}

void PasswordExec::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_)
            return;
        stopped_ = true;
    }
    cv_.notify_all();
    for (auto &th : workers_) {
        if (th.joinable())
            th.join();
    }
}

void PasswordExec::Loop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&]() { return stopped_ || !q_.empty(); });
            if (stopped_)
                return;
            fn = std::move(q_.front());
            q_.pop_front();
        }
        fn();
    }
}
