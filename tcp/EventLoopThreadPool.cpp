/**
 * @file EventLoopThreadPool.cpp
 * @brief 启动 N 个 EventLoopThread，nextloop() 轮询分配新连接
 */

#include "EventLoopThreadPool.h"
#include "EventLoop.h"
#include "EventLoopThread.h"
#include <memory>

EventLoopThreadPool::EventLoopThreadPool(EventLoop *loop)
    : main_reactor_(loop), thread_nums_(0), next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() {}

void EventLoopThreadPool::start() {
    for (int i = 0; i < thread_nums_; ++i) {
        auto ptr = std::make_unique<EventLoopThread>();
        threads_.push_back(std::move(ptr));
        loops_.push_back(threads_.back()->StartLoop());
    }
}

EventLoop *EventLoopThreadPool::nextloop() {
    EventLoop *ret = main_reactor_;
    if (!loops_.empty()) {
        ret = loops_[next_++];
        if (next_ == static_cast<int>(loops_.size()))
            next_ = 0;
    }
    return ret;
}

void EventLoopThreadPool::SetThreadNums(int thread_nums) { thread_nums_ = thread_nums; }
