#include "TcpConnection.h"
#include "Buffer.h"
#include "Channel.h"
#include "common.h"
#include "EventLoop.h"
#include "HttpContext.h"
#include "TimeStamp.h"
#include "Logging.h"
#include <memory>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/sendfile.h>

TcpConnection::TcpConnection(EventLoop *loop, int connfd, uint64_t connid)
    : connfd_(connfd),
      connid_(connid),
      state_(ConnectionState::Invalid),
      loop_(loop),
      max_read_buf_bytes_(static_cast<size_t>(kDefaultMaxReadBufBytes)),
      max_send_buf_bytes_(static_cast<size_t>(kDefaultMaxSendBufBytes)),
      force_closed_for_backpressure_(false),
      in_close_(false) {
    if (loop != nullptr) {
        channel_ = std::make_unique<Channel>(connfd, loop);
        channel_->set_read_callback(std::bind(&TcpConnection::HandleMessage, this));
        channel_->set_write_callback(std::bind(&TcpConnection::HandleWrite, this));
    }
    read_buf_ = std::make_unique<Buffer>();
    send_buf_ = std::make_unique<Buffer>();
    context_ = std::make_unique<HttpContext>();
}

TcpConnection::~TcpConnection() {
    ::close(connfd_);
}

void TcpConnection::ConnectionEstablished() {
    state_ = ConnectionState::Connected;
    int nodelay = 1;
    ::setsockopt(connfd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    channel_->Tie(shared_from_this());
    // 先跑 on_connect_（限流/摘流可能立刻 HandleClose）。未通过时不要
    // EnableRead：限流洪泛下 Channel 进 epoll 再关，会 SIGSEGV。
    if (on_connect_)
        on_connect_(shared_from_this());
    if (state_ != ConnectionState::Connected)
        return;
    // 水平触发。EPOLLET 在「write 返回 EAGAIN 之后、注册 EPOLLOUT 之前窗口又打开」
    // 时不会再通知，send_buf 里的 EnterMapRsp 会一直留到关闭，关闭时若接收缓冲还有
    // 客户端后续请求，close() 会发 RST。
    channel_->EnableRead();
}

void TcpConnection::ConnectionDestructor() {
    proto_stream_.clear();
    if (read_buf_)
        read_buf_->RetrieveAll();
    if (send_buf_)
        send_buf_->RetrieveAll();
    if (!channel_ || !channel_->IsInEpoll())
        return;
    channel_->DisableAll();
    loop_->DeleteChannel(channel_.get());
}

void TcpConnection::set_connection_callback(
    std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn) {
    on_connect_ = std::move(fn);
}
void TcpConnection::set_close_callback(
    std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn) {
    on_close_ = std::move(fn);
}
void TcpConnection::set_message_callback(
    std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn) {
    on_message_ = std::move(fn);
}

void TcpConnection::HandleClose() {
    if (state_ == ConnectionState::Disconected || in_close_)
        return;
    in_close_ = true;
    // 持有 self，避免 on_close_ 链路里最后一次 shared_ptr 释放导致中途析构。
    const auto self = shared_from_this();
    // 先摘掉 epoll 兴趣，避免同一轮 EPOLLIN|EPOLLOUT 在读关闭后再进写回调。
    if (channel_)
        channel_->DisableAll();

    // 尽力冲一次发送队列；关闭路径禁止再递归进 HandleClose。
    FlushSendBufBestEffort();
    if (send_buf_ && send_buf_->readablebytes() > 0) {
        LOG_WARN << "TcpConnection close drops unsent id#" << connid_
                 << " bytes=" << send_buf_->readablebytes();
        send_buf_->RetrieveAll();
    }
    if (connfd_ >= 0) {
        char dump[4096];
        while (true) {
            const ssize_t n = ::read(connfd_, dump, sizeof(dump));
            if (n > 0)
                continue;
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
    }
    state_ = ConnectionState::Disconected;
    proto_stream_.clear();
    if (on_close_)
        on_close_(self);
}

void TcpConnection::CloseForBackpressure(const char *reason) {
    force_closed_for_backpressure_ = true;
    LOG_WARN << "TcpConnection close for backpressure id#" << connid_ << " fd#" << connfd_ << " "
             << (reason ? reason : "");
    HandleClose();
}

void TcpConnection::HandleMessage() {
    if (state_ != ConnectionState::Connected || in_close_)
        return;
    Read();
    if (state_ != ConnectionState::Connected || in_close_)
        return;
    if (on_message_)
        on_message_(shared_from_this());
}

void TcpConnection::HandleWrite() {
    if (state_ != ConnectionState::Connected || in_close_)
        return;
    WriteNonBlocking();
}

EventLoop *TcpConnection::loop() const { return loop_; }
int TcpConnection::fd() const { return connfd_; }
uint64_t TcpConnection::id() const { return connid_; }
TcpConnection::ConnectionState TcpConnection::state() const { return state_; }
Buffer *TcpConnection::read_buf() { return read_buf_.get(); }
Buffer *TcpConnection::send_buf() { return send_buf_.get(); }
std::string &TcpConnection::proto_stream() { return proto_stream_; }
void TcpConnection::set_max_read_buf_bytes(size_t n) { max_read_buf_bytes_ = n; }
void TcpConnection::set_max_send_buf_bytes(size_t n) { max_send_buf_bytes_ = n; }
size_t TcpConnection::max_read_buf_bytes() const { return max_read_buf_bytes_; }
size_t TcpConnection::max_send_buf_bytes() const { return max_send_buf_bytes_; }
bool TcpConnection::force_closed_for_backpressure() const {
    return force_closed_for_backpressure_;
}
bool TcpConnection::IsWriting() const { return channel_ && channel_->IsWriting(); }

void TcpConnection::Send(const std::string &msg) {
    Send(msg.data(), static_cast<int>(msg.size()));
}

void TcpConnection::Send(const char *msg) {
    Send(msg, static_cast<int>(strlen(msg)));
}

void TcpConnection::Send(const char *msg, int len) {
    (void)SendInternal(msg, len, false);
}

bool TcpConnection::TrySend(const std::string &msg) {
    return TrySend(msg.data(), static_cast<int>(msg.size()));
}

bool TcpConnection::TrySend(const char *msg, int len) {
    return SendInternal(msg, len, true);
}

bool TcpConnection::TrySendUrgent(const std::string &msg) {
    return TrySendUrgent(msg.data(), static_cast<int>(msg.size()));
}

bool TcpConnection::TrySendUrgent(const char *msg, int len) {
    // soft_drop=true 且 urgent：SendInternal 用 90% 水位。
    return SendInternal(msg, len, true, true);
}

bool TcpConnection::TrySendControl(const std::string &msg) {
    return TrySendControl(msg.data(), static_cast<int>(msg.size()));
}

bool TcpConnection::TrySendControl(const char *msg, int len) {
    return SendInternal(msg, len, true, true, true);
}

bool TcpConnection::SendInternal(const char *msg, int len, bool soft_drop, bool urgent,
                                 bool control) {
    if (state_ != ConnectionState::Connected || in_close_ || !channel_ || !msg || len < 0)
        return false;

    int remaining = len;
    int send_size = 0;

    if (send_buf_->readablebytes() == 0) {
        send_size = static_cast<int>(::write(connfd_, msg, static_cast<size_t>(len)));
        if (send_size >= 0) {
            remaining -= send_size;
        } else if (errno == EINTR) {
            send_size = 0;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            send_size = 0;
        } else {
            LOG_ERROR << "TcpConnection::Send write error id#" << connid_;
            HandleClose();
            return false;
        }
    }

    if (remaining > 0) {
        // 普通可丢帧 50%；ENTER/LEAVE 等 urgent 用 90%；心跳 control 用满缓冲。都不 Hard Close。
        size_t limit = max_send_buf_bytes_;
        if (soft_drop && !control) {
            limit = urgent ? (max_send_buf_bytes_ - max_send_buf_bytes_ / 10)
                           : (max_send_buf_bytes_ / 2);
        }
        if (static_cast<size_t>(send_buf_->readablebytes()) + static_cast<size_t>(remaining) >
            limit) {
            if (soft_drop)
                return false;
            CloseForBackpressure("send_queue_limit");
            return false;
        }
        send_buf_->Append(msg + send_size, remaining);
    }
    if (send_buf_->readablebytes() > 0) {
        WriteNonBlocking();
        if (state_ == ConnectionState::Connected && !in_close_ && channel_ &&
            send_buf_->readablebytes() > 0)
            channel_->EnableWrite();
    }
    return true;
}

void TcpConnection::Read() { ReadNonBlocking(); }

void TcpConnection::Write() { WriteNonBlocking(); }

void TcpConnection::ReadNonBlocking() {
    if (state_ != ConnectionState::Connected || in_close_ || connfd_ < 0)
        return;
    char buf[1024];
    while (true) {
        ssize_t bytes_read = ::read(connfd_, buf, sizeof(buf));
        if (bytes_read > 0) {
            read_buf_->Append(buf, static_cast<int>(bytes_read));
            if (static_cast<size_t>(read_buf_->readablebytes()) > max_read_buf_bytes_) {
                CloseForBackpressure("read_buf_limit");
                break;
            }
        } else if (bytes_read == -1 && errno == EINTR) {
            continue;
        } else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (bytes_read == 0) {
            HandleClose();
            break;
        } else {
            HandleClose();
            break;
        }
    }
}

void TcpConnection::FlushSendBufBestEffort() {
    if (!send_buf_ || connfd_ < 0)
        return;
    while (send_buf_->readablebytes() > 0) {
        const int remaining = send_buf_->readablebytes();
        const ssize_t send_size =
            ::write(connfd_, send_buf_->Peek(), static_cast<size_t>(remaining));
        if (send_size > 0) {
            send_buf_->Retrieve(static_cast<int>(send_size));
            continue;
        }
        if (send_size == -1 && errno == EINTR)
            continue;
        // EAGAIN / 硬错误：关闭路径不再递归 HandleClose，由调用方丢弃剩余字节。
        break;
    }
}

void TcpConnection::WriteNonBlocking() {
    if (!send_buf_ || !channel_ || state_ == ConnectionState::Disconected || in_close_)
        return;
    while (send_buf_->readablebytes() > 0) {
        const int remaining = send_buf_->readablebytes();
        const ssize_t send_size =
            ::write(connfd_, send_buf_->Peek(), static_cast<size_t>(remaining));
        if (send_size > 0) {
            send_buf_->Retrieve(static_cast<int>(send_size));
            continue;
        }
        if (send_size == -1 && errno == EINTR)
            continue;
        if (send_size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        LOG_ERROR << "TcpConnection::WriteNonBlocking error id#" << connid_;
        HandleClose();
        return;
    }
    if (state_ != ConnectionState::Connected || in_close_ || !channel_)
        return;
    if (send_buf_->readablebytes() == 0 && channel_->IsWriting())
        channel_->DisableWrite();
}

HttpContext *TcpConnection::context() const { return context_.get(); }

TimeStamp TcpConnection::timestamp() const { return timestamp_; }
void TcpConnection::UpdateTimeStamp(TimeStamp now) { timestamp_ = now; }

void TcpConnection::SendFile(int filefd, int size) {
    ssize_t send_size = 0;
    ssize_t data_size = static_cast<ssize_t>(size);
    while (send_size < data_size) {
        ssize_t bytes_write =
            sendfile(connfd_, filefd, (off_t *)&send_size, static_cast<size_t>(data_size - send_size));
        if (bytes_write == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        send_size += bytes_write;
    }
}
