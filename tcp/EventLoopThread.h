#pragma once

/**
 * @file EventLoopThread.h
 * @brief 一个后台线程运行一个 EventLoop（用于 IO 线程池中的单个 worker）
 */

#include "common.h"

#include <mutex>
#include <thread>
#include <condition_variable>

class EventLoop;

class EventLoopThread {
public:
    DISALLOW_COPY_AND_MOVE(EventLoopThread)
    EventLoopThread();
    ~EventLoopThread();

    /** 启动线程并阻塞直到子线程创建好 EventLoop，返回其指针 */
    EventLoop *StartLoop();

private:
    void ThreadFunc();

    EventLoop *loop_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
