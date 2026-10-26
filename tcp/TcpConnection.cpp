#include "TcpConnection.h"
#include "Buffer.h"
#include "Channel.h"
#include "common.h"
#include "EventLoop.h"
#include "HttpContext.h"
#include "ServerStats.h"
#include "TimeStamp.h"
#include "Logging.h"
#include <algorithm>
#include <memory>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>

#ifndef SIOCOUTQNSD
#define SIOCOUTQNSD 0x894B
#endif

// 内核里还没交给网卡的字节超过这个值时，最新移动帧先留在应用缓冲，不立刻 write。
// 再写进去的话，移动回包会排在这批旧坐标后面。只统计还没离开本机的字节。
constexpr int kLatestMoveKernelBudget = 1024;
// 1KB 低于一条忙连接的正常在途量。只按它放行，最新位置会停在应用缓冲里几分钟。
// 200ms 定时器在这个范围内把已经停放的那一帧写进内核。超过它仍继续停放。
constexpr int kLatestMoveReleaseBudget = 64 * 1024;

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
    if (connfd_ >= 0) {
        // 接收缓冲里还有客户端的移动包时，直接 close 会发 RST，
        // 对端看到「远程主机强迫关闭」后就不再发 Move。
        char dump[4096];
        while (true) {
            const ssize_t n = ::read(connfd_, dump, sizeof(dump));
            if (n > 0)
                continue;
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        ::shutdown(connfd_, SHUT_WR);
        ::close(connfd_);
        connfd_ = -1;
    }
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
void TcpConnection::set_drain_callback(std::function<void()> fn) { on_drain_ = std::move(fn); }

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
int TcpConnection::unsent_bytes() const { return UnsentBytes(); }
uint64_t TcpConnection::id() const { return connid_; }
TcpConnection::ConnectionState TcpConnection::state() const { return state_; }
Buffer *TcpConnection::read_buf() { return read_buf_.get(); }
Buffer *TcpConnection::send_buf() { return send_buf_.get(); }

std::string TcpConnection::CopySendBuf() {
    if (!send_buf_)
        return {};
    return send_buf_->PeekAllAsString();
}

void TcpConnection::ReplaceSendBuf(const std::string &bytes) {
    if (!send_buf_)
        return;
    send_buf_->RetrieveAll();
    if (!bytes.empty())
        send_buf_->Append(bytes);
    send_partial_prefix_ = 0;
    partial_is_control_ = false;
    control_run_ = 0;
    control_hold_end_ = 0;
    send_hold_move_ = false;
    send_move_hold_off_ = 0;
}

size_t TcpConnection::send_partial_prefix() const { return send_partial_prefix_; }

size_t TcpConnection::control_run() const { return control_run_; }

size_t TcpConnection::control_hold_end() const { return control_hold_end_; }

void TcpConnection::RefreshControlHoldEnd() {
    if (!send_buf_ || send_buf_->readablebytes() <= 0) {
        send_partial_prefix_ = 0;
        partial_is_control_ = false;
        control_run_ = 0;
        control_hold_end_ = 0;
        return;
    }
    if (send_partial_prefix_ == 0)
        partial_is_control_ = false;
    if (control_run_ == 0 && !partial_is_control_) {
        control_hold_end_ = 0;
        return;
    }
    // 半帧前缀在前，完整控制帧紧跟其后。两段字节不重叠，结束位置只加一次。
    size_t end = send_partial_prefix_ + control_run_;
    const size_t queued = static_cast<size_t>(send_buf_->readablebytes());
    if (end > queued)
        end = queued;
    control_hold_end_ = end;
}

void TcpConnection::ReplaceQueuedAfterPrefix(const std::string &queued) {
    if (!send_buf_)
        return;
    const std::string raw = send_buf_->PeekAllAsString();
    const size_t prefix = std::min(send_partial_prefix_, raw.size());
    std::string next;
    next.reserve(prefix + queued.size());
    next.append(raw.data(), prefix);
    next.append(queued);
    send_buf_->RetrieveAll();
    if (!next.empty())
        send_buf_->Append(next);
    send_partial_prefix_ = prefix;
    if (prefix == 0)
        partial_is_control_ = false;
    if (control_run_ > queued.size())
        control_run_ = queued.size();
    send_hold_move_ = false;
    send_move_hold_off_ = 0;
    const int queued_n = send_buf_->readablebytes();
    if (queued_n <= 0)
        control_hold_end_ = 0;
    else if (control_hold_end_ > static_cast<size_t>(queued_n))
        control_hold_end_ = static_cast<size_t>(queued_n);
    if (send_buf_->readablebytes() == 0) {
        send_partial_prefix_ = 0;
        partial_is_control_ = false;
        control_run_ = 0;
        control_hold_end_ = 0;
    } else {
        RefreshControlHoldEnd();
    }
}

void TcpConnection::ConsumeSendPrefix(int n) {
    if (n <= 0 || send_partial_prefix_ == 0)
        return;
    if (static_cast<size_t>(n) >= send_partial_prefix_)
        send_partial_prefix_ = 0;
    else
        send_partial_prefix_ -= static_cast<size_t>(n);
}

void TcpConnection::NoteWriteConsumed(const char *data, size_t data_len, size_t written) {
    if (!data || written == 0 || written >= data_len) {
        send_partial_prefix_ = 0;
        return;
    }
    const size_t prefix = std::min(send_partial_prefix_, data_len);
    if (written < prefix) {
        send_partial_prefix_ = prefix - written;
        return;
    }
    size_t into = written - prefix;
    size_t off = prefix;
    while (into > 0 && off + 4 <= data_len) {
        uint32_t be = 0;
        std::memcpy(&be, data + off, 4);
        const uint32_t flen = ntohl(be);
        if (flen == 0 || flen > 16u * 1024u * 1024u) {
            send_partial_prefix_ = 0;
            return;
        }
        const size_t frame = static_cast<size_t>(4u + flen);
        const size_t have = data_len - off;
        if (into < frame && into < have) {
            send_partial_prefix_ = std::min(frame, have) - into;
            return;
        }
        if (have < frame) {
            send_partial_prefix_ = into < have ? have - into : 0;
            return;
        }
        into -= frame;
        off += frame;
    }
    send_partial_prefix_ = 0;
}

int TcpConnection::UnsentBytes() const {
    int n = 0;
    if (connfd_ < 0)
        return 0;
    // 只要还没写进内核发送队列的字节。TIOCOUTQ 把已经在路上的未确认字节也算进去，
    // 链路本身有几十 KB 在飞时会把后续移动全部丢掉。
    if (::ioctl(connfd_, SIOCOUTQNSD, &n) != 0 || n < 0)
        return 0;
    return n;
}
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

TcpConnection::ControlSendResult TcpConnection::TrySendControl(const std::string &msg, bool over_cap) {
    return TrySendControl(msg.data(), static_cast<int>(msg.size()), over_cap);
}

TcpConnection::ControlSendResult TcpConnection::TrySendControl(const char *msg, int len,
                                                               bool over_cap) {
    if (state_ != ConnectionState::Connected || in_close_ || !channel_ || !msg || len < 0)
        return Closed;
    if (!SendInternal(msg, len, true, true, true, false, over_cap)) {
        if (state_ != ConnectionState::Connected || in_close_)
            return Closed;
        return Blocked;
    }
    if (!send_buf_ || send_buf_->readablebytes() == 0) {
        control_hold_end_ = 0;
        partial_is_control_ = false;
        return WrittenDirect;
    }
    RefreshControlHoldEnd();
    // 已经按 control_run_ 排进发送流。缓冲里还有控制字节也继续接受下一条。
    return control_hold_end_ > 0 ? Buffered : WrittenDirect;
}

bool TcpConnection::TrySendLatestMove(const std::string &msg) {
    if (state_ != ConnectionState::Connected || in_close_ || !send_buf_ || msg.empty())
        return false;
    const int queued = send_buf_->readablebytes();
    // 非移动数据已经堆过 64KB，这一帧先不追加。调用方留着位置，200ms 后再试。
    if (queued > 64 * 1024)
        return false;
    // 调用方已经清掉缓冲里的旧移动帧。内核里还有没送出的字节时，不要再 write：
    // 回包插不到已经进内核的字节前面，客户端会一直读旧坐标，步子越来越慢直到停下。
    // 这一帧留在缓冲末尾，下一轮先丢掉再换成更新的位置，所以不会一帧帧叠上去。
    if (UnsentBytes() > kLatestMoveKernelBudget) {
        send_buf_->Append(msg);
        send_move_hold_off_ = static_cast<size_t>(queued);
        send_hold_move_ = true;
        // 移动帧前面若还有回包，注册写事件把回包送走；只剩移动帧时不注册，避免空转。
        ArmWriteIfNeeded();
        ArmParkedMoveRetry();
        return true;
    }
    send_hold_move_ = false;
    send_move_hold_off_ = 0;
    return SendInternal(msg.data(), static_cast<int>(msg.size()), true, false, false, true);
}

bool TcpConnection::SendInternal(const char *msg, int len, bool soft_drop, bool urgent,
                                 bool control, bool ignore_unsent_cap, bool control_over_cap) {
    if (state_ != ConnectionState::Connected || in_close_ || !channel_ || !msg || len < 0)
        return false;

    // 心跳和请求回包插到已经排好的 MOVE 前面，但要排在更早的回包后面。
    // 已经写进内核的字节挪不走。应用缓冲开头若是半帧，必须先把这半帧写完。
    // 缓冲里的非移动数据超过上限时，这一帧先不写进缓冲，由调用方排队，不能静默丢掉。
    const int control_cap = control_over_cap ? (256 * 1024) : (64 * 1024);
    if (control && send_buf_ && send_buf_->readablebytes() > control_cap)
        return false;

    if (control && send_buf_ && send_buf_->readablebytes() > 0 && len > 0) {
        const std::string raw = send_buf_->PeekAllAsString();
        const size_t prefix = std::min(send_partial_prefix_, raw.size());
        size_t insert_at = prefix + control_run_;
        if (insert_at > raw.size())
            insert_at = raw.size();
        std::string next;
        next.reserve(raw.size() + static_cast<size_t>(len));
        next.append(raw.data(), insert_at);
        next.append(msg, static_cast<size_t>(len));
        next.append(raw.data() + insert_at, raw.size() - insert_at);
        send_buf_->RetrieveAll();
        send_buf_->Append(next);
        control_run_ += static_cast<size_t>(len);
        if (send_hold_move_ && send_move_hold_off_ >= insert_at)
            send_move_hold_off_ += static_cast<size_t>(len);
        WriteNonBlocking();
        ArmWriteIfNeeded();
        return true;
    }

    // 移动包只看「还没离开本机的字节」：内核未发送队列 + 本连接缓冲。
    // 超过水位直接丢掉这一帧。已经在路上的未确认字节不占这个水位。
    // 最新位置由 TrySendLatestMove 在内核未发送字节还多时先压住，不在这里按 32KB 整帧丢掉。
    // 应用缓冲里的非移动数据已经超过 64KB 时不再追加。
    if (!control && soft_drop && send_buf_) {
        if (ignore_unsent_cap) {
            if (send_buf_->readablebytes() > 64 * 1024)
                return false;
        } else {
            const int pending = UnsentBytes() + send_buf_->readablebytes();
            const int cap = urgent ? (32 * 1024) : (8 * 1024);
            if (pending > 0 && pending + len > cap)
                return false;
        }
    }

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
            // 软丢和最新移动都不关连接。人还在图上时拆连接，客户端就停在最后一帧。
            if (soft_drop || ignore_unsent_cap)
                return false;
            CloseForBackpressure("send_queue_limit");
            return false;
        }
        const bool was_empty = send_buf_->readablebytes() == 0;
        send_buf_->Append(msg + send_size, remaining);
        // 只有已经把帧头写进内核时，缓冲里剩下的才是半帧。一次都没写出的完整帧
        // 不能算半帧，否则心跳会被插到这整帧后面。
        if (was_empty && send_size > 0) {
            send_partial_prefix_ = static_cast<size_t>(remaining);
            partial_is_control_ = control;
        } else if (control && was_empty && send_size == 0) {
            control_run_ += static_cast<size_t>(remaining);
        }
    }
    if (send_buf_->readablebytes() > 0) {
        WriteNonBlocking();
        ArmWriteIfNeeded();
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
            NoteWriteConsumed(send_buf_->Peek(), static_cast<size_t>(remaining),
                              static_cast<size_t>(send_size));
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
    const int before = send_buf_->readablebytes();
    bool control_released = false;
    while (send_buf_->readablebytes() > 0) {
        int writable = send_buf_->readablebytes();
        bool hold_limit = false;
        if (send_hold_move_ && UnsentBytes() > kLatestMoveKernelBudget) {
            const size_t front =
                std::min(send_move_hold_off_, static_cast<size_t>(writable));
            // 前面没有回包或半帧时，最新移动先留着，等内核队列掉下来再写。
            if (front == 0)
                break;
            writable = static_cast<int>(front);
            hold_limit = true;
        }
        const ssize_t send_size =
            ::write(connfd_, send_buf_->Peek(), static_cast<size_t>(writable));
        if (send_size > 0) {
            const size_t old_prefix = send_partial_prefix_;
            const size_t old_run = control_run_;
            const size_t old_hold = control_hold_end_;
            NoteWriteConsumed(send_buf_->Peek(), static_cast<size_t>(send_buf_->readablebytes()),
                              static_cast<size_t>(send_size));
            if (old_run > 0 && static_cast<size_t>(send_size) > old_prefix) {
                const size_t into = static_cast<size_t>(send_size) - old_prefix;
                const size_t consumed = std::min(old_run, into);
                size_t run_left = old_run - consumed;
                // 没写完的那一截被记进半帧前缀。它原来也在 control_run_ 里，这里减掉，避免结束位置加两次。
                if (send_partial_prefix_ > 0 && run_left > 0 && into < old_run) {
                    const size_t overlap = std::min(send_partial_prefix_, run_left);
                    run_left -= overlap;
                    partial_is_control_ = true;
                } else if (send_partial_prefix_ > 0 && into < old_run) {
                    partial_is_control_ = true;
                } else if (send_partial_prefix_ > 0 && into >= old_run) {
                    partial_is_control_ = false;
                } else {
                    partial_is_control_ = false;
                }
                control_run_ = run_left;
            } else if (send_partial_prefix_ == 0) {
                partial_is_control_ = false;
            }
            if (send_move_hold_off_ > 0) {
                if (static_cast<size_t>(send_size) >= send_move_hold_off_)
                    send_move_hold_off_ = 0;
                else
                    send_move_hold_off_ -= static_cast<size_t>(send_size);
            }
            if (!hold_limit)
                send_hold_move_ = false;
            send_buf_->Retrieve(static_cast<int>(send_size));
            RefreshControlHoldEnd();
            // 控制字节离开应用缓冲就结束等待，不等后面的 MOVE 写出。
            if (old_hold > 0 && control_hold_end_ == 0)
                control_released = true;
            if (control_released && send_buf_->readablebytes() > 0)
                break;
            if (hold_limit)
                break;
            continue;
        }
        if (send_size == -1 && errno == EINTR)
            continue;
        if (send_size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ServerStats::tcp_eagain.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        LOG_ERROR << "TcpConnection::WriteNonBlocking error id#" << connid_;
        ServerStats::tcp_write_error.fetch_add(1, std::memory_order_relaxed);
        HandleClose();
        return;
    }
    {
        const int unsent = UnsentBytes();
        if (unsent > 0) {
            uint64_t prev = ServerStats::tcp_conn_sendq_max.load(std::memory_order_relaxed);
            const uint64_t now = static_cast<uint64_t>(unsent);
            while (now > prev && !ServerStats::tcp_conn_sendq_max.compare_exchange_weak(
                                     prev, now, std::memory_order_relaxed)) {
            }
        }
    }
    const auto notify_drain = [&]() {
        if (!on_drain_ || !send_buf_ || before <= send_buf_->readablebytes())
            return;
        on_drain_();
    };
    if (state_ != ConnectionState::Connected || in_close_ || !channel_ || !send_buf_)
        return;
    if (send_buf_->readablebytes() == 0) {
        send_hold_move_ = false;
        send_move_hold_off_ = 0;
        send_partial_prefix_ = 0;
        partial_is_control_ = false;
        control_run_ = 0;
        control_hold_end_ = 0;
        if (channel_->IsWriting())
            channel_->DisableWrite();
        notify_drain();
        return;
    }
    // 只剩压住的最新移动帧时关掉写兴趣。水平触发下一直可写会空转。
    if (send_hold_move_ && send_move_hold_off_ == 0 &&
        UnsentBytes() > kLatestMoveKernelBudget && channel_->IsWriting())
        channel_->DisableWrite();
    if (send_hold_move_)
        ArmParkedMoveRetry();
    notify_drain();
}

void TcpConnection::ArmParkedMoveRetry() {
    if (move_retry_armed_ || !loop_ || !send_hold_move_)
        return;
    if (state_ != ConnectionState::Connected || in_close_)
        return;
    move_retry_armed_ = true;
    std::weak_ptr<TcpConnection> weak = shared_from_this();
    loop_->RunAfter(0.2, [weak]() {
        auto live = weak.lock();
        if (!live)
            return;
        live->OnParkedMoveTimer();
    });
}

void TcpConnection::OnParkedMoveTimer() {
    move_retry_armed_ = false;
    if (state_ != ConnectionState::Connected || in_close_ || !send_buf_ || !send_hold_move_)
        return;
    if (UnsentBytes() <= kLatestMoveReleaseBudget) {
        send_hold_move_ = false;
        WriteNonBlocking();
        ArmWriteIfNeeded();
    }
    if (send_hold_move_)
        ArmParkedMoveRetry();
}

void TcpConnection::ArmWriteIfNeeded() {
    if (!channel_ || state_ != ConnectionState::Connected || in_close_ || !send_buf_)
        return;
    if (send_buf_->readablebytes() == 0) {
        if (channel_->IsWriting())
            channel_->DisableWrite();
        return;
    }
    if (send_hold_move_ && send_move_hold_off_ == 0 &&
        UnsentBytes() > kLatestMoveKernelBudget) {
        if (channel_->IsWriting())
            channel_->DisableWrite();
        return;
    }
    channel_->EnableWrite();
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
