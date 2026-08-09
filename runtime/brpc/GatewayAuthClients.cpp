#include "GatewayAuthClients.h"

#include "BrpcNamingUtil.h"
#include "BrpcSslUtil.h"
#include "GatewayConfigPath.h"
#include "Logging.h"

#include <brpc/channel.h>

GatewayAuthClients &GatewayAuthClients::Instance() {
    static GatewayAuthClients g;
    return g;
}

bool GatewayAuthClients::InitAuthSession(const std::string &session_addr_or_csv, int timeout_ms) {
    std::vector<std::string> addrs;
    if (session_addr_or_csv.rfind("list://", 0) == 0) {
        addrs.push_back(session_addr_or_csv);
    } else {
        GatewayConfigPath::SplitCsv(session_addr_or_csv, &addrs);
    }
    return InitAuthSession(addrs, timeout_ms);
}

bool GatewayAuthClients::InitAuthSession(const std::vector<std::string> &session_addrs,
                                         int timeout_ms) {
    timeout_ms_ = timeout_ms;
    session_channel_.reset();
    session_peer_count_ = 0;
    if (session_addrs.empty())
        return false;
    const std::string naming = BuildListNamingUrl(session_addrs);
    session_channel_ = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.timeout_ms = timeout_ms_;
    opt.max_retry = 0;  // Acquire/Reconnect 等变更禁止自动重试
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::ApplyChannel(&opt, ssl);
    const std::string lb = SessionLoadBalancerName(session_addrs.size());
    const int rc = lb.empty() ? session_channel_->Init(naming.c_str(), &opt)
                              : session_channel_->Init(naming.c_str(), lb.c_str(), &opt);
    if (rc != 0) {
        LOG_ERROR << "GatewayAuthClients session Init failed naming=" << naming;
        session_channel_.reset();
        return false;
    }
    session_peer_count_ = session_addrs.size();
    LOG_INFO << "GatewayAuthClients session ready naming=" << naming
             << " peers=" << session_peer_count_ << " lb=" << (lb.empty() ? "none" : lb);
    return true;
}

bool GatewayAuthClients::InitLogicChannels(const std::vector<std::string> &logic_addrs,
                                           const std::vector<std::string> &logic_ids,
                                           int timeout_ms) {
    timeout_ms_ = timeout_ms;
    logic_channels_.clear();
    for (size_t i = 0; i < logic_addrs.size(); ++i) {
        auto ch = std::make_shared<brpc::Channel>();
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

std::shared_ptr<brpc::Channel> GatewayAuthClients::SharedLogicChannel(const std::string &id) {
    // fail-closed：未知 logic_server_id 绝不回退到首节点
    if (id.empty())
        return nullptr;
    auto it = logic_channels_.find(id);
    if (it != logic_channels_.end())
        return it->second;
    LOG_ERROR << "GatewayAuthClients: unknown logic_server_id=" << id;
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

bool GatewayAuthClients::AuthRegister(const auth::RegisterRequest &req, auth::RegisterResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    auth::AuthService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.Register(&cntl, &req, rsp, nullptr);
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

bool GatewayAuthClients::ReconnectV2(const sess::ReconnectRequest &req,
                                     sess::ReconnectResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.ReconnectV2(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::GetSessionOperation(const sess::GetSessionOperationRequest &req,
                                             sess::GetSessionOperationResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.GetSessionOperation(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::LogoutV2(const sess::LogoutRequest &req, sess::LogoutResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.LogoutV2(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::BeginPlayerTransfer(const sess::BeginPlayerTransferRequest &req,
                                             sess::BeginPlayerTransferResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.BeginPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::CommitPlayerTransfer(const sess::CommitPlayerTransferRequest &req,
                                              sess::CommitPlayerTransferResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.CommitPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::AbortPlayerTransfer(const sess::AbortPlayerTransferRequest &req,
                                             sess::AbortPlayerTransferResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.AbortPlayerTransfer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::GetPlayerRoute(const sess::GetPlayerRouteRequest &req,
                                        sess::GetPlayerRouteResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.GetPlayerRoute(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                                           sess::UpdatePlayerRouteResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.UpdatePlayerRoute(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ResolveOrCreateMap(const sess::ResolveOrCreateMapRequest &req,
                                            sess::ResolveOrCreateMapResponse *rsp) {
    if (!session_channel_ || !rsp)
        return false;
    sess::SessionService_Stub stub(session_channel_.get());
    brpc::Controller cntl;
    stub.ResolveOrCreateMap(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::BindPlayer(const std::string &logic_instance_id,
                                    const glrpc::BindPlayerRequest &req,
                                    glrpc::BindPlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.BindPlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::UnbindPlayer(const std::string &logic_instance_id,
                                      const glrpc::UnbindPlayerRequest &req,
                                      glrpc::UnbindPlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.UnbindPlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool GatewayAuthClients::FreezePlayer(const std::string &logic_instance_id,
                                      const glrpc::FreezePlayerRequest &req,
                                      glrpc::FreezePlayerResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.FreezePlayer(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ExportPlayerSnapshot(const std::string &logic_instance_id,
                                              const glrpc::ExportPlayerSnapshotRequest &req,
                                              glrpc::ExportPlayerSnapshotResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.ExportPlayerSnapshot(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::ImportPlayerSnapshot(const std::string &logic_instance_id,
                                              const glrpc::ImportPlayerSnapshotRequest &req,
                                              glrpc::ImportPlayerSnapshotResponse *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.ImportPlayerSnapshot(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool GatewayAuthClients::Dispatch(const std::string &logic_instance_id,
                                  const glrpc::ClientCommand &req, glrpc::CommandResult *rsp) {
    auto ch = SharedLogicChannel(logic_instance_id);
    if (!ch || !rsp)
        return false;
    glrpc::GameLogicService_Stub stub(ch.get());
    brpc::Controller cntl;
    stub.Dispatch(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}
