#include "GatewayPushServer.h"

#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "ProtoFraming.h"
#include "game.pb.h"
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
    const std::string &local_gw = GatewayPushServer::Instance().gateway_instance_id();
    if (!local_gw.empty() && !request->gateway_instance_id().empty() &&
        request->gateway_instance_id() != local_gw) {
        response->set_ok(false);
        response->set_message("gateway_instance_id mismatch");
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
        if (!local_gw.empty() && !bind.gateway_instance_id.empty() &&
            bind.gateway_instance_id != local_gw) {
            ++rejected;
            continue;
        }
        if (!m.fence_token().empty() && !bind.token.empty() && m.fence_token() != bind.token) {
            ++rejected;
            continue;
        }
        if (m.generation() != 0 && bind.generation != 0 && m.generation() != bind.generation) {
            ++rejected;
            continue;
        }
        std::string raw = m.payload();
        if (raw.size() < 4) {
            ++rejected;
            continue;
        }
        const uint32_t be = (static_cast<uint8_t>(raw[0]) << 24) |
                            (static_cast<uint8_t>(raw[1]) << 16) |
                            (static_cast<uint8_t>(raw[2]) << 8) |
                            static_cast<uint8_t>(raw[3]);
        std::string inner_body;
        if (be + 4 == raw.size())
            inner_body = raw.substr(4);
        else
            inner_body = raw;
        // 客户端可见 Envelope：带真实 server_seq（兼容：seq=0 仍发原业务帧）
        std::string frame;
        if (m.server_seq() != 0) {
            game::GameResponse env;
            env.set_ok(true);
            env.set_message("server_push");
            auto *p = env.mutable_server_push();
            p->set_server_seq(m.server_seq());
            p->set_message_type(m.message_type());
            p->set_payload(inner_body);
            p->set_reliable(m.reliable());
            p->set_coalescable(m.coalescable());
            std::string body;
            if (!env.SerializeToString(&body) || !gameproto::EncodeFrame(body, &frame)) {
                ++rejected;
                continue;
            }
        } else if (be + 4 == raw.size()) {
            frame = std::move(raw);
        } else {
            if (!gameproto::EncodeFrame(inner_body, &frame)) {
                ++rejected;
                continue;
            }
        }
        if (!GatewayConnRegistry::Instance().SendBySession(m.session_id(), frame)) {
            ++rejected;
            continue;
        }
        // 权威回放在 Redis PushReplayStore；此处不再双写本地缓存
        ++accepted;
    }
    response->set_ok(true);
    response->set_accepted(accepted);
    response->set_rejected(rejected);
    response->set_message("ok");
}

void GatewayPushServiceImpl::KickConnection(::google::protobuf::RpcController *controller,
                                            const ::gwpush::KickConnectionRequest *request,
                                            ::gwpush::KickConnectionResponse *response,
                                            ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_message("invalid kick");
        return;
    }
    const bool closed = GatewayConnRegistry::Instance().CloseIfMatch(
        request->player_id(), request->session_id(), request->generation());
    response->set_ok(true);
    response->set_closed(closed);
    response->set_message(closed ? "closed" : "not_found");
    LOG_INFO << "KickConnection player=" << request->player_id()
             << " session=" << request->session_id() << " gen=" << request->generation()
             << " closed=" << closed;
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
    LOG_INFO << "GatewayPushServer listening on " << listen_addr_
             << " gw=" << gateway_instance_id_;
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
