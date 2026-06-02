#pragma once

/**
 * @file EventLoopThreadPool.h
 * @brief IO 线程池：主 reactor + 多个子 EventLoop，新连接 round-robin 分配
 */

#include "common.h"

#include <memory>
#include <thread>
#include <vector>

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
public:
    DISALLOW_COPY_AND_MOVE(EventLoopThreadPool);
    EventLoopThreadPool(EventLoop *loop);
    ~EventLoopThreadPool();

    void SetThreadNums(int thread_nums);
    void start();

    /** 轮询返回下一个子 loop；无子线程时返回 main_reactor_ */
    EventLoop *nextloop();

private:
    EventLoop *main_reactor_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop *> loops_;
    int thread_nums_;
    int next_;
};
