/**
 * @file Channel.cpp
 * @brief 根据 epoll 返回的 ready_events_ 分发读/写回调
 */

#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>
#include <utility>

Channel::Channel(int fd, EventLoop *loop)
    : fd_(fd),
      loop_(loop),
      listen_events_(0),
      ready_events_(0),
      in_epoll_(false),
      tied_(false) {}

Channel::~Channel() {}

void Channel::HandleEvent() const {
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)
            HandleEventWithGuard();
    } else {
        HandleEventWithGuard();
    }
}

void Channel::Tie(const std::shared_ptr<void> &ptr) {
    tied_ = true;
    tie_ = ptr;
}

void Channel::HandleEventWithGuard() const {
    if (ready_events_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_callback_)
            read_callback_();
    }
    if (ready_events_ & EPOLLOUT) {
        if (write_callback_)
            write_callback_();
    }
}

void Channel::EnableRead() {
    listen_events_ |= (EPOLLIN | EPOLLPRI);
    loop_->UpdateChannel(this);
}

void Channel::DisableRead() {
    listen_events_ &= static_cast<short>(~(EPOLLIN | EPOLLPRI));
    loop_->UpdateChannel(this);
}

void Channel::EnableWrite() {
    listen_events_ |= EPOLLOUT;
    loop_->UpdateChannel(this);
}

void Channel::DisableWrite() {
    listen_events_ &= static_cast<short>(~EPOLLOUT);
    loop_->UpdateChannel(this);
}

void Channel::EnableET() {
    listen_events_ |= EPOLLET;
    loop_->UpdateChannel(this);
}

int Channel::fd() const { return fd_; }
short Channel::listen_events() const { return listen_events_; }
short Channel::ready_events() const { return ready_events_; }
bool Channel::IsWriting() const { return (listen_events_ & EPOLLOUT) != 0; }
bool Channel::IsInEpoll() const { return in_epoll_; }
void Channel::SetInEpoll(bool in) { in_epoll_ = in; }
void Channel::SetReadyEvents(int ev) { ready_events_ = ev; }

void Channel::set_read_callback(std::function<void()> const &callback) {
    read_callback_ = std::move(callback);
}
void Channel::set_write_callback(std::function<void()> const &callback) {
    write_callback_ = std::move(callback);
}
