#pragma once

/**
 * @file EventLoop.h
 * @brief 事件循环：单线程 reactor 核心（epoll + 定时器 + 跨线程任务队列）
 *
 * 每个 IO 线程一个 EventLoop，线程内：
 *   Loop() -> epoll_wait -> 分发 Channel::HandleEvent -> DoToDoList()
 *
 * 跨线程安全接口：
 *   QueueOneFunc / RunOneFunc：其他线程向本 loop 投递任务（经 eventfd 唤醒）
 */

#include "common.h"

#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <vector>

class Epoller;
class TimerQueue;
class TimeStamp;

class EventLoop {
public:
    DISALLOW_COPY_AND_MOVE(EventLoop);
    EventLoop();
    ~EventLoop();

    /** 阻塞运行：poll -> 处理 IO -> 执行 to_do_list_ */
    void Loop();
    void UpdateChannel(Channel *ch);
    void DeleteChannel(Channel *ch);

    void RunAt(TimeStamp timestamp, std::function<void()> const &cb);
    void RunAfter(double wait_time, std::function<void()> const &cb);
    void RunEvery(double interval, std::function<void()> const &cb);

    /** 本轮 poll 结束后执行队列中的 functor */
    void DoToDoList();

    /** 将任务加入 to_do_list_；非本线程则 write eventfd 唤醒 */
    void QueueOneFunc(std::function<void()> fn);

    /** 本线程则立即执行，否则等价于 QueueOneFunc */
    void RunOneFunc(std::function<void()> fn);

    bool IsInLoopThread();

    /** eventfd 可读：消费计数并 DoToDoList */
    void HandleRead();

private:
    std::unique_ptr<Epoller> poller_;
    std::vector<std::function<void()>> to_do_list_;
    std::mutex mutex_;

    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;

    bool calling_functors_;
    pid_t tid_;

    std::unique_ptr<TimerQueue> timer_queue_;
};
