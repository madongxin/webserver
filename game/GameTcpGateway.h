#pragma once

/**
 * @file GameTcpGateway.h
 * @brief 游戏 TCP 网关：独立线程 + EventLoop，接收客户端 length-prefix protobuf 消息
 *
 * 接收客户端消息的整体链路（自底向上）：
 *
 *   1. Acceptor 在新连接上 accept
 *   2. TcpServer::HandleNewConnection 将连接绑到子线程 EventLoop，注册读回调
 *   3. epoll 可读 -> TcpConnection::HandleMessage
 *        -> ReadNonBlocking 把内核数据写入 read_buf_
 *        -> 调用 message_callback（即本类的 OnMessage）
 *   4. OnMessage：把本次 read_buf_ 追加到「按连接 id 的流式缓冲」g_stream_buf
 *   5. 循环 ProtoFraming::TryDecodeOneFrame，拆出完整一帧 payload
 *   6. 按 player_id 投递 PlayerSerialQueue（InProcessTransport）
 *   7. 业务线程：GameService::HandleFrame -> ReplySink 回投连接所属 EventLoop -> Send
 *
 * 协议：4 字节大端长度 + protobuf 序列化的 GameRequest / GameResponse（见 ProtoFraming）
 *
 * 注意：网关运行在独立 std::thread 中（StartInBackground），与 HTTP 主 EventLoop 分离。
 * 阶段 1：IO 线程不再同步跑 GameLogic。
 * 阶段 3：Login/Reconnect 绑定 conn↔player；断线 MarkDisconnected（宽限重连）。
 */

#include <memory>
#include <string>
#include <thread>

class TcpConnection;

class GameTcpGateway {
public:
    GameTcpGateway(const std::string &ip, int port);
    ~GameTcpGateway();

    /** 在后台线程启动 Run()，内部阻塞在 EventLoop::Loop */
    void StartInBackground();

private:
    void Run();
    /** TcpServer::set_message_callback 注册的入口：粘包处理 + 业务 + 回包 */
    void OnMessage(const std::shared_ptr<TcpConnection> &conn);

    std::string ip_;
    int port_ = 0;
    std::thread thread_;
};
