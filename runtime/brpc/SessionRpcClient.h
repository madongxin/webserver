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
                        uint64_t connection_id);
    bool MarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

    bool ResolveOrCreateMap(const sess::ResolveOrCreateMapRequest &req,
                            sess::ResolveOrCreateMapResponse *rsp);
    bool GetPlacement(uint64_t map_instance_id, sess::GetPlacementResponse *rsp);
    bool MigrateMap(const sess::MigrateMapRequest &req, sess::MigrateMapResponse *rsp);
    bool MarkRecovering(uint64_t map_instance_id, const std::string &reason,
                        sess::MarkRecoveringResponse *rsp);
    bool UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                           sess::UpdatePlayerRouteResponse *rsp);

private:
    SessionRpcClient() = default;
    std::unique_ptr<brpc::Channel> channel_;
};
