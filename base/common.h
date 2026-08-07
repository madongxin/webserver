#pragma once

class Epoller;
class Channel;
class Buffer;
class EventLoop;
class TcpConnection;
class ThreadPool;
class Acceptor;
class TcpServer;

class HttpServer;
class HttpContext;
class HttpRequest;
class HttpResponse;
 

// Macros to disable copying and moving（与 butil/brpc 共存时勿重复定义）
#ifndef DISALLOW_COPY
#define DISALLOW_COPY(cname)     \
  cname(const cname &) = delete; \
  cname &operator=(const cname &) = delete;
#endif

#ifndef DISALLOW_MOVE
#define DISALLOW_MOVE(cname) \
  cname(cname &&) = delete;  \
  cname &operator=(cname &&) = delete;
#endif

#ifndef DISALLOW_COPY_AND_MOVE
#define DISALLOW_COPY_AND_MOVE(cname) \
  DISALLOW_COPY(cname);               \
  DISALLOW_MOVE(cname);
#endif

// #define ASSERT(expr, message) assert((expr) && (message))

// #define UNREACHABLE(message) throw std::logic_error(message)

enum RC {
  RC_UNDEFINED,
  RC_SUCCESS,
  RC_SOCKET_ERROR,
  RC_POLLER_ERROR,
  RC_CONNECTION_ERROR,
  RC_ACCEPTOR_ERROR,
  RC_UNIMPLEMENTED
};