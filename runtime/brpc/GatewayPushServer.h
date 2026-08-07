#pragma once

#include "gateway_push.pb.h"

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class GatewayPushServiceImpl : public gwpush::GatewayPushService {
public:
    void PushBatch(::google::protobuf::RpcController *controller,
                   const ::gwpush::PushBatchRequest *request,
                   ::gwpush::PushBatchResponse *response,
                   ::google::protobuf::Closure *done) override;
};

class GatewayPushServer {
public:
    static GatewayPushServer &Instance();
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);
    void Stop();
    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    GatewayPushServer() = default;
    ~GatewayPushServer();
    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<GatewayPushServiceImpl> service_;
    std::string listen_addr_;
    bool running_ = false;
};
