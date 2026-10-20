#include "TcpReplySink.h"

#include "EventLoop.h"
#include "Logging.h"
#include "OpsMetrics.h"
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
        if (c->state() != TcpConnection::Connected) {
            LOG_WARN << "TcpReplySink drop frame conn#" << c->id()
                     << " bytes=" << response_frame.size()
                     << " state=" << static_cast<int>(c->state());
            return;
        }
        // 登录/重连/进图回包：用满发送缓冲。90% 水位会把应答丢掉，客户端只能看到 CLIENT_TIMEOUT。
        if (!c->TrySendControl(response_frame)) {
            OpsMetrics::Instance().IncReplySoftDrop();
            LOG_WARN << "TcpReplySink soft-drop reply conn#" << c->id()
                     << " bytes=" << response_frame.size();
            return;
        }
        OpsMetrics::Instance().AddGatewayTxBytes(response_frame.size());
        OpsMetrics::Instance().AddGatewayTxFrames(1);
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
