#pragma once

/**
 * @file TcpServer.h
 * @brief TCP 服务入口：Reactor 多线程模型（主从 EventLoop）
 *
 * =============================================================================
 * 模块架构（自上而下）
 * =============================================================================
 *
 *   TcpServer
 *     ├── Acceptor          主 reactor 上 listen，accept 新 fd
 *     ├── EventLoopThreadPool  N 个子 reactor（IO 线程）
 *     └── connectionsMap_   fd -> TcpConnection（主线程维护表）
 *
 *   每个 TcpConnection
 *     ├── Channel           把 connfd 注册到所属 EventLoop 的 epoll
 *     ├── Buffer read_buf_  读缓冲
 *     ├── Buffer send_buf_  写缓冲（写不完时 EPOLLOUT 继续写）
 *     └── 回调 on_message_ / on_close_ / on_connect_
 *
 *   EventLoop（每线程一个）
 *     ├── Epoller           epoll_wait
 *     ├── Channel 列表      通过 UpdateChannel 增删改
 *     ├── eventfd 唤醒      跨线程 QueueOneFunc
 *     └── TimerQueue        RunEvery / RunAfter（HTTP 定时器等）
 *
 * =============================================================================
 * 典型流程
 * =============================================================================
 *
 * 【启动】
 *   TcpServer(loop, ip, port) -> Acceptor 绑定监听
 *   Start() -> thread_pool_->start() 起子线程 EventLoop
 *            -> main_reactor_->Loop()  主线程 epoll 循环（只 accept）
 *
 * 【新连接】
 *   listenfd 可读 -> Acceptor::AcceptConnection -> accept4 非阻塞
 *   -> HandleNewConnection(fd)
 *        -> nextloop() 轮询选一个子 reactor
 *        -> new TcpConnection(sub_loop, fd, id)
 *        -> connectionsMap_[fd]=conn, ConnectionEstablished()
 *        -> 子 reactor 上 connfd EPOLLIN（ET）
 *
 * 【收数据】（以 GameTcpGateway 为例）
 *   connfd 可读 -> TcpConnection::HandleMessage
 *        -> ReadNonBlocking -> read_buf_
 *        -> on_message_(conn)  // 业务层拆包、处理、Send 响应
 *
 * 【发数据】
 *   Send() -> 先 write；若 EAGAIN 或未写完 -> append send_buf_ + EnableWrite
 *   EPOLLOUT -> HandleWrite -> WriteNonBlocking
 *
 * 【关闭】
 *   read==0 或错误 -> HandleClose -> on_close_
 *   -> HandleClose 投递到主 loop -> HandleCloseInLoop 从 map 删除
 *   -> 子 loop QueueOneFunc(ConnectionDestructor) -> DeleteChannel
 *
 * =============================================================================
 * 设计要点
 * =============================================================================
 * - 主 reactor 只做 accept + 连接表管理，避免与子线程争用 map
 * - 一连接固定在一个子 EventLoop，无跨线程操作 conn（除关闭时投递）
 * - 非阻塞 socket + EPOLLET：读事件需循环 read 直到 EAGAIN
 * - Channel::Tie(shared_ptr) 防止回调执行中 TcpConnection 被析构
 */

#include "common.h"
#include <functional>
#include <map>
#include <vector>
#include <memory>

class EventLoop;
class TcpConnection;
class Acceptor;
class EventLoopThreadPool;
class InetAddress;

class TcpServer {
public:
    DISALLOW_COPY_AND_MOVE(TcpServer);
    TcpServer(EventLoop *loop, const char *ip, const int port);
    ~TcpServer();

    /** 启动子 IO 线程池，并在 main_reactor_ 上进入 Loop（阻塞） */
    void Start();

    void set_connection_callback(std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);
    void set_message_callback(std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);
    /** 连接从 map 移除前调用（主 reactor 线程） */
    void set_disconnect_callback(
        std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);

    /** 子线程连接关闭时回调；再投递到主 loop 删 map */
    inline void HandleClose(const std::shared_ptr<TcpConnection> &);
    inline void HandleCloseInLoop(const std::shared_ptr<TcpConnection> &);

    /** Acceptor 回调：为新 fd 创建 TcpConnection 并注册到子 reactor */
    inline void HandleNewConnection(int fd);

    /** 设置子 EventLoop 数量（0 表示仅主 reactor 处理 IO，本项目中通常 >0） */
    void SetThreadNums(int thread_nums);

private:
    EventLoop *main_reactor_;
    int next_conn_id_;

    std::unique_ptr<EventLoopThreadPool> thread_pool_;
    std::unique_ptr<Acceptor> acceptor_;
    std::map<int, std::shared_ptr<TcpConnection>> connectionsMap_;

    std::function<void(const std::shared_ptr<TcpConnection> &)> on_connect_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_message_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_disconnect_;
};
