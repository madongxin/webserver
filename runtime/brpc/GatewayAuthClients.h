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

    bool InitAuthSession(const std::string &session_addr, int timeout_ms = 3000);
    bool InitLogicChannels(const std::vector<std::string> &logic_addrs,
                           const std::vector<std::string> &logic_ids, int timeout_ms = 3000);
    bool ready() const { return session_channel_ != nullptr; }

    bool AuthLogin(const auth::LoginRequest &req, auth::LoginResponse *rsp);
    bool AcquireSession(const sess::AcquireSessionRequest &req, sess::AcquireSessionResponse *rsp);
    bool BindPlayer(const std::string &logic_instance_id, const glrpc::BindPlayerRequest &req,
                    glrpc::BindPlayerResponse *rsp);
    bool UnbindPlayer(const std::string &logic_instance_id, const glrpc::UnbindPlayerRequest &req,
                      glrpc::UnbindPlayerResponse *rsp);
    bool Dispatch(const std::string &logic_instance_id, const glrpc::ClientCommand &req,
                  glrpc::CommandResult *rsp);

private:
    GatewayAuthClients() = default;
    brpc::Channel *LogicChannel(const std::string &id);

    std::unique_ptr<brpc::Channel> session_channel_;
    std::unordered_map<std::string, std::unique_ptr<brpc::Channel>> logic_channels_;
    std::vector<std::unique_ptr<brpc::Channel>> logic_by_index_;
    int timeout_ms_ = 3000;
};
