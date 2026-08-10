#pragma once

#include "game.pb.h"
#include "session.pb.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/** Gateway/Logic/World → 独立 Session 进程；未 Init 时调用方应走本地 SessionStore */
class SessionRpcClient {
public:
    static SessionRpcClient &Instance();

    /** 单地址或 CSV；多地址使用 list:// + rr，不得只取首地址 */
    bool Init(const std::string &addr_or_csv, int timeout_ms = 3000);
    bool Init(const std::vector<std::string> &addrs, int timeout_ms = 3000);
    bool ready() const;
    size_t peer_count() const;
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
    bool HeartbeatOwner(uint64_t map_instance_id, const std::string &owner_logic_id,
                        uint64_t owner_epoch, uint32_t lease_sec, int64_t *lease_until_out);
    bool UpdatePlayerRoute(const sess::UpdatePlayerRouteRequest &req,
                           sess::UpdatePlayerRouteResponse *rsp);

private:
    SessionRpcClient() = default;
    std::shared_ptr<brpc::Channel> CurrentChannel() const;

    std::shared_ptr<brpc::Channel> channel_;
    std::atomic<size_t> peer_count_{0};
};
