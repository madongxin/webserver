#pragma once

#include "game.pb.h"
#include "session.pb.h"

#include <memory>
#include <string>

namespace brpc {
class Channel;
}

/** Gateway/Logic/World → 独立 Session 进程；未 Init 时调用方应走本地 SessionStore */
class SessionRpcClient {
public:
    static SessionRpcClient &Instance();

    bool Init(const std::string &addr, int timeout_ms = 3000);
    bool ready() const { return channel_ != nullptr; }
    void Shutdown();

    bool Login(const game::LoginReq &req, game::LoginRsp *rsp);
    bool Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp);
    bool Logout(const game::LogoutReq &req, game::LogoutRsp *rsp);
    bool ValidateToken(uint64_t player_id, const std::string &token, std::string *err);
    bool BindConnection(uint64_t player_id, const std::string &token, const std::string &gateway_id,
                        int connection_id);
    bool MarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

private:
    SessionRpcClient() = default;
    std::unique_ptr<brpc::Channel> channel_;
};
