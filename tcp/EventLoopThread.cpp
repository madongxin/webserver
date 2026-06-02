/**
 * @file EventLoopThread.cpp
 * @brief 子线程创建 EventLoop 并 Loop；StartLoop 用条件变量同步返回 loop 指针
 */

#include "EventLoopThread.h"
#include "EventLoop.h"
#include <mutex>
#include <thread>

EventLoopThread::EventLoopThread() : loop_(nullptr) {}

EventLoopThread::~EventLoopThread() {}

EventLoop *EventLoopThread::StartLoop() {
    thread_ = std::thread(std::bind(&EventLoopThread::ThreadFunc, this));

    EventLoop *loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr)
            cv_.wait(lock);
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::ThreadFunc() {
    EventLoop loop;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cv_.notify_one();
    }

    loop.Loop();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = nullptr;
    }
}
