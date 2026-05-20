#include "GameTcpGateway.h"

#include "Buffer.h"
#include "EventLoop.h"
#include "GameService.h"
#include "Logging.h"
#include "ProtoFraming.h"
#include "TcpConnection.h"
#include "TcpServer.h"

#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

std::mutex g_stream_mu;
std::unordered_map<int, std::string> g_stream_buf;

std::string &StreamBuf(int conn_id) {
    return g_stream_buf[conn_id];
}

}  // namespace

GameTcpGateway::GameTcpGateway(const std::string &ip, int port) : ip_(ip), port_(port) {}

GameTcpGateway::~GameTcpGateway() {
    if (thread_.joinable())
        thread_.join();
}

void GameTcpGateway::StartInBackground() {
    thread_ = std::thread([this]() { Run(); });
}

void GameTcpGateway::Run() {
    EventLoop loop;
    auto server = std::make_unique<TcpServer>(&loop, ip_.c_str(), port_);
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers <= 1)
        workers = 1;
    else
        --workers;
    server->SetThreadNums(workers);
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    // TcpServer/Acceptor 构造时已 bind；失败会 LOG_FATAL 退出
    LOG_INFO << "GameTcpGateway ready on " << ip_ << ":" << port_
             << " (length-prefix + protobuf)";
    server->Start();
}

void GameTcpGateway::OnMessage(const std::shared_ptr<TcpConnection> &conn) {
    Buffer *rb = conn->read_buf();
    if (!rb || rb->readablebytes() <= 0)
        return;
    std::lock_guard<std::mutex> lk(g_stream_mu);
    std::string &buf = StreamBuf(conn->id());
    buf.append(rb->RetrieveAllAsString());
    std::string frame;
    while (gameproto::TryDecodeOneFrame(&buf, &frame)) {
        std::string out;
        if (!gameproto::HandleFrame(frame, &out)) {
            LOG_ERROR << "GameTcpGateway: HandleFrame failed conn#" << conn->id();
            continue;
        }
        conn->Send(out);
    }
}
