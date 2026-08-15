#pragma once

#include "gateway_push.pb.h"

#include <string>

/** GameLogic → 按 gateway_instance_id 选择目标 Gateway，非广播。 */
namespace GameLogicPush {

bool PushToBoundGateway(const std::string &gateway_instance_id, uint64_t player_id,
                       const std::string &session_id, const std::string &message_type,
                       const std::string &payload_or_frame, bool reliable, bool coalescable,
                       uint64_t server_seq, const std::string &fence_token = "",
                       uint64_t generation = 0);

}  // namespace GameLogicPush
