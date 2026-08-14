#pragma once

#include "SessionHandle.h"

#include <functional>
#include <string>

namespace gameproto {

/**
 * 跨 GameLogic EnterMap 编排（在非 Reactor 线程调用）：
 * Resolve → BeginTransfer → Freeze(old) → Bind/Prepare(new) → Commit → Dispatch EnterMap → Unbind(old)
 * 同 Owner 时直接 Dispatch，并在成功后刷新 sticky。
 */
bool OrchestrateGatewayEnterMap(const SessionHandle &sticky, const std::string &request_payload,
                                std::string *response_frame, SessionHandle *route_out);

using GatewayEnterMapDone =
    std::function<void(bool ok, std::string response_frame, SessionHandle route)>;

bool BeginOrchestrateGatewayEnterMap(const SessionHandle &sticky, const std::string &request_payload,
                                     GatewayEnterMapDone done);

}  // namespace gameproto
