#pragma once

/**
 * @file Epoller.h
 * @brief epoll 封装：注册 Channel、等待就绪事件
 *
 * epoll_event.data.ptr 存 Channel*，Poll 后回填 ready_events 到各 Channel
 */

#include "common.h"

#include <vector>
#include <sys/epoll.h>

class Channel;

class Epoller {
public:
    DISALLOW_COPY_AND_MOVE(Epoller);

    Epoller();
    ~Epoller();

    void UpdateChannel(Channel *ch) const;
    void DeleteChannel(Channel *ch) const;

    /** @param timeout_ms epoll_wait 超时（毫秒），EventLoop 传 10000 */
    std::vector<Channel *> Poll(long timeout_ms = -1) const;

private:
    int fd_;
    struct epoll_event *events_;
};
