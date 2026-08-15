#include "TcpReplySink.h"

#include "EventLoop.h"
#include "TcpConnection.h"

TcpReplySink::TcpReplySink(const std::shared_ptr<TcpConnection> &conn)
    : conn_(conn), loop_(conn ? conn->loop() : nullptr) {}

void TcpReplySink::SendFrame(const std::string &response_frame) {
    if (!loop_)
        return;
    auto weak = conn_;
    loop_->QueueOneFunc([weak, response_frame]() {
        auto c = weak.lock();
        if (!c)
            return;
        if (c->state() != TcpConnection::Connected)
            return;
        c->Send(response_frame);
    });
}

void TcpReplySink::CloseConnection() {
    if (!loop_)
        return;
    auto weak = conn_;
    loop_->QueueOneFunc([weak]() {
        auto c = weak.lock();
        if (!c)
            return;
        if (c->state() != TcpConnection::Connected)
            return;
        c->HandleClose();
    });
}

std::shared_ptr<TcpConnection> TcpReplySink::tcp_connection() const {
    return conn_.lock();
}
