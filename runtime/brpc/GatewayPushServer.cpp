#include "GatewayPushServer.h"

#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "ProtoFraming.h"

#include <brpc/controller.h>
#include <brpc/server.h>

void GatewayPushServiceImpl::PushBatch(::google::protobuf::RpcController *controller,
                                       const ::gwpush::PushBatchRequest *request,
                                       ::gwpush::PushBatchResponse *response,
                                       ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request) {
        response->set_ok(false);
        response->set_message("null request");
        return;
    }
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    for (const auto &m : request->messages()) {
        GatewayConnRegistry::Bind bind;
        if (!GatewayConnRegistry::Instance().FindBySession(m.session_id(), &bind)) {
            ++rejected;
            continue;
        }
        if (bind.player_id != 0 && m.player_id() != 0 && bind.player_id != m.player_id()) {
            ++rejected;
            continue;
        }
        // payload：若已是完整帧则直送；否则当 GameResponse body 封装
        std::string frame = m.payload();
        if (frame.size() < 4) {
            ++rejected;
            continue;
        }
        // 简单判定：若前 4 字节像长度头且匹配剩余长度，视为已成帧
        const uint32_t be = (static_cast<uint8_t>(frame[0]) << 24) |
                            (static_cast<uint8_t>(frame[1]) << 16) |
                            (static_cast<uint8_t>(frame[2]) << 8) |
                            static_cast<uint8_t>(frame[3]);
        if (be + 4 != frame.size()) {
            std::string wrapped;
            if (!gameproto::EncodeFrame(frame, &wrapped)) {
                ++rejected;
                continue;
            }
            frame = std::move(wrapped);
        }
        if (!GatewayConnRegistry::Instance().SendBySession(m.session_id(), frame)) {
            ++rejected;
            continue;
        }
        ++accepted;
    }
    response->set_ok(true);
    response->set_accepted(accepted);
    response->set_rejected(rejected);
    response->set_message("ok");
}

GatewayPushServer &GatewayPushServer::Instance() {
    static GatewayPushServer g;
    return g;
}

GatewayPushServer::~GatewayPushServer() { Stop(); }

bool GatewayPushServer::Start(const std::string &listen_addr, int idle_timeout_sec) {
    if (running_)
        return true;
    server_.reset(new brpc::Server());
    service_.reset(new GatewayPushServiceImpl());
    if (server_->AddService(service_.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG_ERROR << "GatewayPushServer AddService failed";
        return false;
    }
    brpc::ServerOptions options;
    options.idle_timeout_sec = idle_timeout_sec;
    if (server_->Start(listen_addr.c_str(), &options) != 0) {
        LOG_ERROR << "GatewayPushServer Start failed " << listen_addr;
        return false;
    }
    listen_addr_ = listen_addr;
    running_ = true;
    LOG_INFO << "GatewayPushServer listening on " << listen_addr_;
    return true;
}

void GatewayPushServer::Stop() {
    if (!server_)
        return;
    server_->Stop(0);
    server_->Join();
    server_.reset();
    service_.reset();
    running_ = false;
}
