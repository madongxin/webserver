/**
 * @file TcpServer.cpp
 * @brief TcpServer：accept 在主线，连接 IO 在子 EventLoop
 */

#include "TcpServer.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "Logging.h"
#include "TcpConnection.h"
#include <cassert>

TcpServer::TcpServer(EventLoop *loop, const char *ip, const int port)
    : main_reactor_(loop), next_conn_id_(1) {
  acceptor_ = std::make_unique<Acceptor>(main_reactor_, ip, port);
  acceptor_->set_newconnection_callback(
      std::bind(&TcpServer::HandleNewConnection, this, std::placeholders::_1));
  thread_pool_ = std::make_unique<EventLoopThreadPool>(loop);
}

TcpServer::~TcpServer() {}

void TcpServer::Start() {
  thread_pool_->start();
  main_reactor_->Loop();
}

void TcpServer::HandleNewConnection(int fd) {
  assert(fd != -1);

  EventLoop *sub_reactor = thread_pool_->nextloop();

  auto conn = std::make_shared<TcpConnection>(sub_reactor, fd, next_conn_id_);
  conn->set_connection_callback(on_connect_);
  conn->set_close_callback(
      std::bind(&TcpServer::HandleClose, this, std::placeholders::_1));
  conn->set_message_callback(on_message_);

  connectionsMap_[fd] = conn;

  ++next_conn_id_;
  if (next_conn_id_ == 1000)
    next_conn_id_ = 1;

  conn->ConnectionEstablished();
}

void TcpServer::HandleClose(const std::shared_ptr<TcpConnection> &conn) {
  main_reactor_->RunOneFunc(
      std::bind(&TcpServer::HandleCloseInLoop, this, conn));
}

void TcpServer::HandleCloseInLoop(const std::shared_ptr<TcpConnection> &conn) {
  LOG_INFO << "TcpServer::HandleCloseInLoop - Remove connection [id#"
           << conn->id() << "-fd#" << conn->fd() << "]";
  auto it = connectionsMap_.find(conn->fd());
  assert(it != connectionsMap_.end());
  connectionsMap_.erase(it);

  conn->loop()->QueueOneFunc(
      std::bind(&TcpConnection::ConnectionDestructor, conn));
}

void TcpServer::set_connection_callback(
    std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn) {
  on_connect_ = std::move(fn);
}

void TcpServer::set_message_callback(
    std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn) {
  on_message_ = std::move(fn);
}

void TcpServer::SetThreadNums(int thread_nums) {
  thread_pool_->SetThreadNums(thread_nums);
}
