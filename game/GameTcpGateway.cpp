/**
 * @file GameTcpGateway.cpp
 * @brief 游戏 TCP 网关实现：粘包缓冲、拆帧、调用 GameService、回写响应
 */

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

/**
 * 每个 TcpConnection::id() 对应一条「未拆完帧」的字节流。
 * TCP 是流式协议，一次 read 可能只有半帧，也可能含多帧，必须跨多次 OnMessage 拼接。
 */
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
    // 独立线程运行 std::thread
    // 在后台线程启动 Run()，内部阻塞在 EventLoop::Loop
    thread_ = std::thread([this]() { Run(); });
}

void GameTcpGateway::Run() {
    EventLoop loop;
    auto server = std::make_unique<TcpServer>(&loop, ip_.c_str(), port_);
    // 子 reactor 数量：与 CPU 核数相关，accept 仍在主 loop，连接分配到 worker loop
    int workers = static_cast<int>(std::thread::hardware_concurrency());
    if (workers <= 1)
        workers = 1;
    else
        --workers;
    server->SetThreadNums(workers);
    // 有新数据可读时，由 TcpConnection::HandleMessage 最终调到 OnMessage
    server->set_message_callback([this](const std::shared_ptr<TcpConnection> &c) { OnMessage(c); });
    LOG_INFO << "GameTcpGateway ready on " << ip_ << ":" << port_
             << " (length-prefix + protobuf)";
    server->Start();  // 阻塞：loop.Loop()
}

void GameTcpGateway::OnMessage(const std::shared_ptr<TcpConnection> &conn) {
    Buffer *rb = conn->read_buf();
    if (!rb || rb->readablebytes() <= 0)
        return;

    // --- 步骤 A：把本轮 epoll 读到的字节并入该连接的流缓冲（粘包/半包）---
    std::lock_guard<std::mutex> lk(g_stream_mu);
    std::string &buf = StreamBuf(conn->id());
    buf.append(rb->RetrieveAllAsString());  // 清空 read_buf_，数据全部进入 buf

    // --- 步骤 B：尽可能从 buf 中拆出完整帧，可能一次回调拆出多帧 ---
    std::string frame;
    while (gameproto::TryDecodeOneFrame(&buf, &frame)) {
        // frame = 一帧的 protobuf 二进制（无长度头），即 GameRequest 序列化结果
        std::string out;
        if (!gameproto::HandleFrame(frame, &out)) {
            LOG_ERROR << "GameTcpGateway: HandleFrame failed conn#" << conn->id();
            continue;  // 本帧解析/业务失败，不断开连接，继续处理后续帧
        }
        // out = [4 字节长度][GameResponse 序列化]，直接 Send 回客户端
        conn->Send(out);
    }
    // 若 buf 里剩余字节不足一帧，留在 g_stream_buf，等待下次 OnMessage
}
