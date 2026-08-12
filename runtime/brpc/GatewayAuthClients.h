#pragma once

#include "RpcChannelSnapshot.h"
#include "auth.pb.h"
#include "gamelogic_rpc.pb.h"
#include "session.pb.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/** Gateway 侧编排客户端：Auth / Session / GameLogic（不可变 Channel 快照）。 */
class GatewayAuthClients {
public:
    static GatewayAuthClients &Instance();

    bool InitAuthSession(const std::string &session_addr_or_csv, int timeout_ms = 3000);
    bool InitAuthSession(const std::vector<std::string> &session_addrs, int timeout_ms = 3000);
    size_t session_peer_count() const;
    bool InitLogicChannels(const std::vector<std::string> &logic_addrs,
                           const std::vector<std::string> &logic_ids, int timeout_ms = 3000);
    bool ready() const;

    std::shared_ptr<const RpcChannelSnapshot> CurrentSnapshot() const;

    bool AuthLogin(const auth::LoginRequest &req, auth::LoginResponse *rsp);
    bool AuthRegister(const auth::RegisterRequest &req, auth::RegisterResponse *rsp);
    bool AcquireSession(const sess::AcquireSessionRequest &req, sess::AcquireSessionResponse *rsp);
    bool ReconnectV2(const sess::ReconnectRequest &req, sess::ReconnectResponse *rsp);
    bool GetSessionOperation(const sess::GetSessionOperationRequest &req,
                             sess::GetSessionOperationResponse *rsp);
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
    /** 异步 UnbindPlayer：不阻塞调用线程；失败仅记日志 */
    void UnbindPlayerAsync(const std::string &logic_instance_id,
                           const glrpc::UnbindPlayerRequest &req);
    bool FreezePlayer(const std::string &logic_instance_id, const glrpc::FreezePlayerRequest &req,
                      glrpc::FreezePlayerResponse *rsp);
    bool ExportPlayerSnapshot(const std::string &logic_instance_id,
                              const glrpc::ExportPlayerSnapshotRequest &req,
                              glrpc::ExportPlayerSnapshotResponse *rsp);
    bool ImportPlayerSnapshot(const std::string &logic_instance_id,
                              const glrpc::ImportPlayerSnapshotRequest &req,
                              glrpc::ImportPlayerSnapshotResponse *rsp);
    bool Dispatch(const std::string &logic_instance_id, const glrpc::ClientCommand &req,
                  glrpc::CommandResult *rsp);

private:
    GatewayAuthClients() = default;
    std::shared_ptr<brpc::Channel> SharedLogicChannel(const std::string &id);
    void Publish(std::shared_ptr<RpcChannelSnapshot> next);

    std::shared_ptr<const RpcChannelSnapshot> snap_{std::make_shared<RpcChannelSnapshot>()};
    std::atomic<uint64_t> version_{0};
    int timeout_ms_ = 3000;
};
