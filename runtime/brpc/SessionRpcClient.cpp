#include "SessionRpcClient.h"

#include "GatewayConfigPath.h"
#include "Logging.h"
#include "BrpcSslUtil.h"
#include "GameMeshPaths.h"

#include <brpc/channel.h>

SessionRpcClient &SessionRpcClient::Instance() {
    static SessionRpcClient g;
    return g;
}

bool SessionRpcClient::Init(const std::string &addr, int timeout_ms) {
    Shutdown();
    if (addr.empty())
        return false;
    auto ch = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.protocol = "baidu_std";
    opt.timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    opt.max_retry = 0;
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
    if (ch->Init(addr.c_str(), &opt) != 0) {
        LOG_ERROR << "SessionRpcClient Init failed " << addr;
        return false;
    }
    channel_ = std::move(ch);
    LOG_INFO << "SessionRpcClient ready addr=" << addr;
    return true;
}

void SessionRpcClient::Shutdown() { channel_.reset(); }

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
                                      const std::string &gateway_id, int connection_id) {
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
