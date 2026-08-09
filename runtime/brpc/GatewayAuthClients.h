#pragma once

#include "auth.pb.h"
#include "gamelogic_rpc.pb.h"
#include "session.pb.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brpc {
class Channel;
}

/** Gateway 侧编排客户端：Auth / Session / GameLogic BindPlayer（复用 Channel）。 */
class GatewayAuthClients {
public:
    static GatewayAuthClients &Instance();

    /** 单地址、CSV 或多地址；多 Session 使用 list:// + rr */
    bool InitAuthSession(const std::string &session_addr_or_csv, int timeout_ms = 3000);
    bool InitAuthSession(const std::vector<std::string> &session_addrs, int timeout_ms = 3000);
    size_t session_peer_count() const { return session_peer_count_; }
    bool InitLogicChannels(const std::vector<std::string> &logic_addrs,
                           const std::vector<std::string> &logic_ids, int timeout_ms = 3000);
    bool ready() const { return session_channel_ != nullptr; }

    bool AuthLogin(const auth::LoginRequest &req, auth::LoginResponse *rsp);
    bool AuthRegister(const auth::RegisterRequest &req, auth::RegisterResponse *rsp);
    bool AcquireSession(const sess::AcquireSessionRequest &req, sess::AcquireSessionResponse *rsp);
    bool ReconnectV2(const sess::ReconnectRequest &req, sess::ReconnectResponse *rsp);
    bool LogoutV2(const sess::LogoutRequest &req, sess::LogoutResponse *rsp);
    bool BeginPlayerTransfer(const sess::BeginPlayerTransferRequest &req,
                             sess::BeginPlayerTransferResponse *rsp);
    bool CommitPlayerTransfer(const sess::CommitPlayerTransferRequest &req,
                              sess::CommitPlayerTransferResponse *rsp);
    bool AbortPlayerTransfer(const sess::AbortPlayerTransferRequest &req,
                             sess::AbortPlayerTransferResponse *rsp);
    bool GetPlayerRoute(const sess::GetPlayerRouteRequest &req, sess::GetPlayerRouteResponse *rsp);
    bool UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                           sess::UpdatePlayerRouteResponse *rsp);
    bool ResolveOrCreateMap(const sess::ResolveOrCreateMapRequest &req,
                            sess::ResolveOrCreateMapResponse *rsp);
    bool BindPlayer(const std::string &logic_instance_id, const glrpc::BindPlayerRequest &req,
                    glrpc::BindPlayerResponse *rsp);
    bool UnbindPlayer(const std::string &logic_instance_id, const glrpc::UnbindPlayerRequest &req,
                      glrpc::UnbindPlayerResponse *rsp);
    bool FreezePlayer(const std::string &logic_instance_id, const glrpc::FreezePlayerRequest &req,
                      glrpc::FreezePlayerResponse *rsp);
    bool Dispatch(const std::string &logic_instance_id, const glrpc::ClientCommand &req,
                  glrpc::CommandResult *rsp);

private:
    GatewayAuthClients() = default;
    brpc::Channel *LogicChannel(const std::string &id);

    std::unique_ptr<brpc::Channel> session_channel_;
    size_t session_peer_count_ = 0;
    std::unordered_map<std::string, std::unique_ptr<brpc::Channel>> logic_channels_;
    std::vector<std::unique_ptr<brpc::Channel>> logic_by_index_;
    int timeout_ms_ = 3000;
};
