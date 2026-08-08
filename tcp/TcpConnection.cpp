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
#include <cstring>
#include <sys/socket.h>
#include <sys/sendfile.h>

TcpConnection::TcpConnection(EventLoop *loop, int connfd, uint64_t connid)
    : connfd_(connfd),
      connid_(connid),
      state_(ConnectionState::Invalid),
      loop_(loop),
      max_read_buf_bytes_(static_cast<size_t>(kDefaultMaxReadBufBytes)),
      max_send_buf_bytes_(static_cast<size_t>(kDefaultMaxSendBufBytes)),
      force_closed_for_backpressure_(false) {
    if (loop != nullptr) {
        channel_ = std::make_unique<Channel>(connfd, loop);
        channel_->EnableET();
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
    channel_->Tie(shared_from_this());
    channel_->EnableRead();
    if (on_connect_) {
        on_connect_(shared_from_this());
    }
}

void TcpConnection::ConnectionDestructor() {
    proto_stream_.clear();
    read_buf_->RetrieveAll();
    send_buf_->RetrieveAll();
    if (channel_ && channel_->IsWriting())
        channel_->DisableWrite();
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
    if (state_ != ConnectionState::Disconected) {
        state_ = ConnectionState::Disconected;
        proto_stream_.clear();
        if (channel_ && channel_->IsWriting())
            channel_->DisableWrite();
        if (on_close_) {
            on_close_(shared_from_this());
        }
    }
}

void TcpConnection::CloseForBackpressure(const char *reason) {
    force_closed_for_backpressure_ = true;
    LOG_WARN << "TcpConnection close for backpressure id#" << connid_ << " fd#" << connfd_ << " "
             << (reason ? reason : "");
    HandleClose();
}

void TcpConnection::HandleMessage() {
    Read();
    if (state_ == ConnectionState::Disconected)
        return;
    if (on_message_) {
        on_message_(shared_from_this());
    }
}

void TcpConnection::HandleWrite() {
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
    if (state_ != ConnectionState::Connected || !msg || len < 0)
        return;

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
            return;
        }
    }

    assert(remaining <= len);
    if (remaining > 0) {
        if (static_cast<size_t>(send_buf_->readablebytes()) + static_cast<size_t>(remaining) >
            max_send_buf_bytes_) {
            CloseForBackpressure("send_queue_limit");
            return;
        }
        send_buf_->Append(msg + send_size, remaining);
        channel_->EnableWrite();
    }
}

void TcpConnection::Read() { ReadNonBlocking(); }

void TcpConnection::Write() { WriteNonBlocking(); }

void TcpConnection::ReadNonBlocking() {
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

void TcpConnection::WriteNonBlocking() {
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
    if (send_buf_->readablebytes() == 0 && channel_->IsWriting()) {
        channel_->DisableWrite();
    }
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
