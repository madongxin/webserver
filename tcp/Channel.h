#pragma once

/**
 * @file Channel.h
 * @brief 封装一个 fd 在 epoll 中的兴趣事件与回调（Reactor 模式中的「事件处理器」）
 *
 * 不把 IO 逻辑写在 Channel 里，只负责：
 *   listen_events_（关心 EPOLLIN/OUT/ET） + read/write_callback_
 *   epoll 返回后 SetReadyEvents -> HandleEvent 调用对应回调
 *
 * Tie(shared_ptr)：回调执行期间持有 TcpConnection，避免 use-after-free
 */

#include "common.h"

#include <functional>
#include <memory>

class EventLoop;

class Channel {
public:
    DISALLOW_COPY_AND_MOVE(Channel);
    Channel(int fd, EventLoop *loop);
    ~Channel();

    void HandleEvent() const;
    void HandleEventWithGuard() const;

    void EnableRead();
    void EnableWrite();
    void EnableET();

    int fd() const;
    short listen_events() const;
    short ready_events() const;

    bool IsInEpoll() const;
    void SetInEpoll(bool in = true);

    void SetReadyEvents(int ev);
    void set_read_callback(std::function<void()> const &callback);
    void set_write_callback(std::function<void()> const &callback);

    void Tie(const std::shared_ptr<void> &ptr);

private:
    int fd_;
    EventLoop *loop_;

    short listen_events_;
    short ready_events_;
    bool in_epoll_{false};
    std::function<void()> read_callback_;
    std::function<void()> write_callback_;

    bool tied_;
    std::weak_ptr<void> tie_;
};
