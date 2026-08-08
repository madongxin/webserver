#pragma once

#include "GatewayAuthPolicy.h"

#include <cstdint>
#include <string>

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

}  // namespace gameproto
