#include "SessionRpcClient.h"

#include "BrpcNamingUtil.h"
#include "BrpcSslUtil.h"
#include "GameMeshPaths.h"
#include "GatewayConfigPath.h"
#include "Logging.h"

#include <brpc/channel.h>

SessionRpcClient &SessionRpcClient::Instance() {
    static SessionRpcClient g;
    return g;
}

bool SessionRpcClient::Init(const std::string &addr_or_csv, int timeout_ms) {
    std::vector<std::string> addrs;
    if (addr_or_csv.rfind("list://", 0) == 0) {
        addrs.push_back(addr_or_csv);
    } else {
        GatewayConfigPath::SplitCsv(addr_or_csv, &addrs);
    }
    return Init(addrs, timeout_ms);
}

bool SessionRpcClient::Init(const std::vector<std::string> &addrs, int timeout_ms) {
    Shutdown();
    if (addrs.empty())
        return false;
    const std::string naming = BuildListNamingUrl(addrs);
    if (naming.empty())
        return false;
    auto ch = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.protocol = "baidu_std";
    opt.timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    // 多 Session：有限重试以便节点宕机时切换（状态修改幂等由业务保证）
    opt.max_retry = addrs.size() > 1 ? 2 : 0;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(GatewayConfigPath::Cnf(), &ssl);
    if (!ssl.enable) {
        std::string session_cnf = "../config/session.cnf";
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/session.cnf", &resolved))
            session_cnf = resolved;
        BrpcSslUtil::LoadFromCnf(session_cnf, &ssl);
    }
    if (BrpcSslUtil::ApplyChannel(&opt, ssl))
        LOG_INFO << "SessionRpcClient SSL enabled";
    const std::string lb = SessionLoadBalancerName(addrs.size());
    const int rc = lb.empty() ? ch->Init(naming.c_str(), &opt)
                              : ch->Init(naming.c_str(), lb.c_str(), &opt);
    if (rc != 0) {
        LOG_ERROR << "SessionRpcClient Init failed naming=" << naming << " lb=" << lb;
        return false;
    }
    channel_ = std::move(ch);
    peer_count_ = addrs.size();
    LOG_INFO << "SessionRpcClient ready naming=" << naming << " peers=" << peer_count_
             << " lb=" << (lb.empty() ? "none" : lb);
    return true;
}

void SessionRpcClient::Shutdown() {
    channel_.reset();
    peer_count_ = 0;
}

bool SessionRpcClient::Login(const game::LoginReq &req, game::LoginRsp *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.Login(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool SessionRpcClient::Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.Reconnect(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool SessionRpcClient::Logout(const game::LogoutReq &req, game::LogoutRsp *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.Logout(&cntl, &req, rsp, nullptr);
    return !cntl.Failed();
}

bool SessionRpcClient::ValidateToken(uint64_t player_id, const std::string &token, std::string *err) {
    if (!channel_)
        return false;
    sess::ValidateTokenReq req;
    sess::ValidateTokenRsp rsp;
    req.set_player_id(player_id);
    req.set_token(token);
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.ValidateToken(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        if (err)
            *err = cntl.ErrorText();
        return false;
    }
    if (!rsp.ok() && err)
        *err = rsp.message();
    return rsp.ok();
}

bool SessionRpcClient::BindConnection(uint64_t player_id, const std::string &token,
                                      const std::string &gateway_id, uint64_t connection_id) {
    if (!channel_)
        return false;
    sess::BindConnectionReq req;
    sess::BindConnectionRsp rsp;
    req.set_player_id(player_id);
    req.set_token(token);
    req.set_gateway_id(gateway_id);
    req.set_connection_id(connection_id);
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.BindConnection(&cntl, &req, &rsp, nullptr);
    return !cntl.Failed() && rsp.ok();
}

bool SessionRpcClient::MarkDisconnected(uint64_t player_id, const std::string &token,
                                        uint64_t generation) {
    if (!channel_)
        return false;
    sess::MarkDisconnectedReq req;
    sess::MarkDisconnectedRsp rsp;
    req.set_player_id(player_id);
    req.set_token(token);
    req.set_generation(generation);
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.MarkDisconnected(&cntl, &req, &rsp, nullptr);
    return !cntl.Failed() && rsp.ok();
}

bool SessionRpcClient::ResolveOrCreateMap(const sess::ResolveOrCreateMapRequest &req,
                                          sess::ResolveOrCreateMapResponse *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.ResolveOrCreateMap(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool SessionRpcClient::GetPlacement(uint64_t map_instance_id, sess::GetPlacementResponse *rsp) {
    if (!channel_ || !rsp)
        return false;
    sess::GetPlacementRequest req;
    req.set_map_instance_id(map_instance_id);
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.GetPlacement(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool SessionRpcClient::MigrateMap(const sess::MigrateMapRequest &req, sess::MigrateMapResponse *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.MigrateMap(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool SessionRpcClient::MarkRecovering(uint64_t map_instance_id, const std::string &reason,
                                      sess::MarkRecoveringResponse *rsp) {
    if (!channel_ || !rsp)
        return false;
    sess::MarkRecoveringRequest req;
    req.set_map_instance_id(map_instance_id);
    req.set_reason(reason);
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.MarkRecovering(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}

bool SessionRpcClient::HeartbeatOwner(uint64_t map_instance_id, const std::string &owner_logic_id,
                                      uint64_t owner_epoch, uint32_t lease_sec,
                                      int64_t *lease_until_out) {
    if (!channel_ || owner_logic_id.empty())
        return false;
    sess::HeartbeatOwnerRequest req;
    req.set_map_instance_id(map_instance_id);
    req.set_owner_logic_server_id(owner_logic_id);
    req.set_owner_epoch(owner_epoch);
    req.set_lease_sec(lease_sec);
    sess::HeartbeatOwnerResponse rsp;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.HeartbeatOwner(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed() || !rsp.ok())
        return false;
    if (lease_until_out)
        *lease_until_out = rsp.lease_until();
    return true;
}

bool SessionRpcClient::UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                                         sess::UpdatePlayerRouteResponse *rsp) {
    if (!channel_ || !rsp)
        return false;
    brpc::Controller cntl;
    sess::SessionService_Stub stub(channel_.get());
    stub.UpdatePlayerRoute(&cntl, &req, rsp, nullptr);
    return !cntl.Failed() && rsp->ok();
}
