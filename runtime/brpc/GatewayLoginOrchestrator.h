#pragma once

#include "GatewayAuthPolicy.h"
#include "GatewayLoginRoute.h"
#include "GatewayLogoutPolicy.h"

#include <cstdint>
#include <functional>
#include <string>

namespace gameproto {

/**
 * Gateway 编排：Auth.Login → Session.AcquireSession → GameLogic.BindPlayer
 * Auth 失败不创建 Session；Bind 失败回滚 Session。
 * 同步版本会阻塞调用线程；生产路径请用 Begin*（RpcOffloadPool + CompleteAsyncInFlight）。
 */
bool OrchestrateGatewayLogin(const std::string &gateway_instance_id, uint64_t connection_id,
                             const std::string &request_payload, std::string *response_frame,
                             GatewayLoginRoute *route_out);

using GatewayAuthDone = std::function<void(bool ok, std::string response_frame, GatewayLoginRoute route)>;

/** 释放 shard：RPC 在 bthread 执行，完成后回投 PlayerSerialQueue。过载返回 false。 */
bool BeginOrchestrateGatewayLogin(const std::string &gateway_instance_id, uint64_t connection_id,
                                  const std::string &request_payload, uint64_t shard_key,
                                  GatewayAuthDone done);

/** Gateway → AuthService.Register → GameDB（不创建 Session） */
bool OrchestrateGatewayRegister(const std::string &request_payload, std::string *response_frame);
bool BeginOrchestrateGatewayRegister(const std::string &request_payload, uint64_t shard_key,
                                     GatewayAuthDone done);

/** Session.ReconnectV2 → GameLogic.BindPlayer，返回完整路由 */
bool OrchestrateGatewayReconnect(const std::string &gateway_instance_id, uint64_t connection_id,
                                 const std::string &request_payload, std::string *response_frame,
                                 GatewayLoginRoute *route_out);
bool BeginOrchestrateGatewayReconnect(const std::string &gateway_instance_id, uint64_t connection_id,
                                      const std::string &request_payload, uint64_t shard_key,
                                      GatewayAuthDone done);

bool OrchestrateGatewayLogout(const std::string &gateway_instance_id, uint64_t connection_id,
                              const std::string &request_payload, std::string *response_frame,
                              GatewayLogoutResult *result = nullptr);

/** Bind 成功但连接已失效时的幂等 Session 补偿（MarkDisconnected，保留宽限） */
void CompensateGatewaySession(uint64_t player_id, const std::string &session_id,
                              const std::string &fence_token, uint64_t generation);

}  // namespace gameproto
