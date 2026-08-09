#pragma once

#include "GatewayAuthPolicy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gameproto {

struct GatewayLoginRoute {
    uint64_t player_id = 0;
    std::string session_id;
    std::string fence_token;
    uint64_t generation = 0;
    std::string gamelogic_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
    bool need_full_snapshot = false;
    uint64_t last_server_seq = 0;
    /** 重连成功且绑定后按序补发的可靠 Push 载荷（已是 GameResponse body） */
    std::vector<std::string> pending_push_payloads;
};

/**
 * Gateway 编排：Auth.Login → Session.AcquireSession → GameLogic.BindPlayer
 * Auth 失败不创建 Session；Bind 失败回滚 Session。
 * 调用方必须在非 Reactor IO 线程执行（如 PlayerSerialQueue worker）。
 */
bool OrchestrateGatewayLogin(const std::string &gateway_instance_id, uint64_t connection_id,
                             const std::string &request_payload, std::string *response_frame,
                             GatewayLoginRoute *route_out);

/** Gateway → AuthService.Register → GameDB（不创建 Session） */
bool OrchestrateGatewayRegister(const std::string &request_payload, std::string *response_frame);

/** Session.ReconnectV2 → GameLogic.BindPlayer，返回完整路由 */
bool OrchestrateGatewayReconnect(const std::string &gateway_instance_id, uint64_t connection_id,
                                 const std::string &request_payload, std::string *response_frame,
                                 GatewayLoginRoute *route_out);

bool OrchestrateGatewayLogout(const std::string &gateway_instance_id, uint64_t connection_id,
                              const std::string &request_payload, std::string *response_frame);

/** Bind 成功但连接已失效时的幂等 Session 补偿（LogoutV2） */
void CompensateGatewaySession(uint64_t player_id, const std::string &session_id,
                              const std::string &fence_token);

}  // namespace gameproto
