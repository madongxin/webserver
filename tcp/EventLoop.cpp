#include "EventLoop.h"

#include "Channel.h"
#include "CurrentThread.h"
#include "Epoller.h"
#include "EventLoopMetrics.h"
#include "Logging.h"
#include "TimerQueue.h"
#include "TimeStamp.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <utility>

namespace {
thread_local EventLoop *t_loopInThisThread = nullptr;
}  // namespace

EventLoop::EventLoop() : calling_functors_(false), tid_(CurrentThread::tid()) {
    if (t_loopInThisThread) {
        LOG_FATAL << "Another EventLoop exists in this thread";
    } else {
        t_loopInThisThread = this;
    }
    poller_ = std::make_unique<Epoller>();
    timer_queue_ = std::make_unique<TimerQueue>(this);
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    wakeup_channel_ = std::make_unique<Channel>(wakeup_fd_, this);
    wakeup_channel_->set_read_callback(std::bind(&EventLoop::HandleRead, this));
    wakeup_channel_->EnableRead();
    UpdateChannel(wakeup_channel_.get());
}

EventLoop::~EventLoop() {
    wakeup_channel_.reset();
    ::close(wakeup_fd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::Loop() {
    while (true) {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const std::vector<Channel *> active = poller_->Poll(10000);
        for (Channel *ch : active)
            ch->HandleEvent();
        DoToDoList();
        const auto t1 = clock::now();
        const double sec =
            std::chrono::duration<double>(t1 - t0).count();
        EventLoopMetrics::RecordTick(sec);
    }
}

void EventLoop::UpdateChannel(Channel *ch) {
    poller_->UpdateChannel(ch);
}

void EventLoop::DeleteChannel(Channel *ch) {
    poller_->DeleteChannel(ch);
}

void EventLoop::RunAt(TimeStamp timestamp, std::function<void()> const &cb) {
    timer_queue_->AddTimer(timestamp, cb, 0.0);
}

void EventLoop::RunAfter(double wait_time, std::function<void()> const &cb) {
    RunAt(TimeStamp::AddTime(TimeStamp::Now(), wait_time), cb);
}

void EventLoop::RunEvery(double interval, std::function<void()> const &cb) {
    timer_queue_->AddTimer(TimeStamp::Now(), cb, interval);
}

void EventLoop::QueueOneFunc(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to_do_list_.push_back(std::move(fn));
    }
    if (!IsInLoopThread()) {
        uint64_t one = 1;
        ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
        (void)n;
    }
}

void EventLoop::RunOneFunc(std::function<void()> fn) {
    if (IsInLoopThread())
        fn();
    else
        QueueOneFunc(std::move(fn));
}

bool EventLoop::IsInLoopThread() {
    return tid_ == CurrentThread::tid();
}

void EventLoop::DoToDoList() {
    std::vector<std::function<void()>> functors;
    calling_functors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(to_do_list_);
    }
    for (auto &f : functors)
        f();
    calling_functors_ = false;
}

void EventLoop::HandleRead() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeup_fd_, &one, sizeof(one));
    (void)n;
    DoToDoList();
}
