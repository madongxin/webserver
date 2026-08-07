#include "GatewayAuthClients.h"

#include "BrpcSslUtil.h"
#include "Logging.h"

#include <brpc/channel.h>

GatewayAuthClients &GatewayAuthClients::Instance() {
    static GatewayAuthClients g;
    return g;
}

bool GatewayAuthClients::InitAuthSession(const std::string &session_addr, int timeout_ms) {
    timeout_ms_ = timeout_ms;
    session_channel_.reset(new brpc::Channel());
    brpc::ChannelOptions opt;
    opt.timeout_ms = timeout_ms_;
    opt.max_retry = 0;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::ApplyChannel(&opt, ssl);
    if (session_channel_->Init(session_addr.c_str(), &opt) != 0) {
        LOG_ERROR << "GatewayAuthClients session Init failed " << session_addr;
        session_channel_.reset();
        return false;
    }
    LOG_INFO << "GatewayAuthClients session ready " << session_addr;
    return true;
}

bool GatewayAuthClients::InitLogicChannels(const std::vector<std::string> &logic_addrs,
                                           const std::vector<std::string> &logic_ids,
                                           int timeout_ms) {
    timeout_ms_ = timeout_ms;
    logic_channels_.clear();
    logic_by_index_.clear();
    for (size_t i = 0; i < logic_addrs.size(); ++i) {
        auto ch = std::make_unique<brpc::Channel>();
        brpc::ChannelOptions opt;
        opt.timeout_ms = timeout_ms_;
        opt.max_retry = 0;
        if (ch->Init(logic_addrs[i].c_str(), &opt) != 0) {
            LOG_ERROR << "GatewayAuthClients logic Init failed " << logic_addrs[i];
            return false;
        }
        const std::string id =
            i < logic_ids.size() ? logic_ids[i] : ("gl-" + std::to_string(i));
        logic_channels_[id] = std::move(ch);
    }
    LOG_INFO << "GatewayAuthClients logic channels=" << logic_channels_.size();
    return !logic_channels_.empty();
}

brpc::Channel *GatewayAuthClients::LogicChannel(const std::string &id) {
    auto it = logic_channels_.find(id);
    if (it != logic_channels_.end())
        return it->second.get();
    if (!logic_channels_.empty())
        return logic_channels_.begin()->second.get();
    return nullptr;
}

bool GatewayAuthClients::AuthLogin(const auth::LoginRequest &req, auth::LoginResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    auth::AuthService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.Login(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::AcquireSession(const sess::AcquireSessionRequest &req,
                                        sess::AcquireSessionResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.AcquireSession(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::BindPlayer(const std::string &logic_instance_id,
                                    const glrpc::BindPlayerRequest &req,
                                    glrpc::BindPlayerResponse *rsp) {
    auto *ch = LogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch);
    brpc::Controller cntl;
    stub.BindPlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::UnbindPlayer(const std::string &logic_instance_id,
                                      const glrpc::UnbindPlayerRequest &req,
                                      glrpc::UnbindPlayerResponse *rsp) {
    auto *ch = LogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch);
    brpc::Controller cntl;
    stub.UnbindPlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::Dispatch(const std::string &logic_instance_id,
                                  const glrpc::ClientCommand &req, glrpc::CommandResult *rsp) {
    auto *ch = LogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch);
    brpc::Controller cntl;
    stub.Dispatch(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}
