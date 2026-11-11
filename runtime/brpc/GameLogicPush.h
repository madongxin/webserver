#pragma once

#include "gateway_push.pb.h"

#include <string>
#include <unordered_map>
#include <vector>

/** GameLogic → 按 gateway_instance_id 选择目标 Gateway，非广播。 */
namespace GameLogicPush {

struct GatewayPushItem {
    std::string gateway_instance_id;
    uint64_t player_id = 0;
    std::string session_id;
    std::string message_type;
    std::string payload;
    bool reliable = false;
    bool coalescable = false;
    uint64_t server_seq = 0;
    std::string fence_token;
    uint64_t generation = 0;
};

bool PushToBoundGateway(const std::string &gateway_instance_id, uint64_t player_id,
                       const std::string &session_id, const std::string &message_type,
                       const std::string &payload_or_frame, bool reliable, bool coalescable,
                       uint64_t server_seq, const std::string &fence_token = "",
                       uint64_t generation = 0);

/** 同一 Gateway 的多名收件人一次 PushBatch。written 记录每个 player 是否已写入发送缓冲。 */
bool PushGrouped(std::vector<GatewayPushItem> items,
                 std::unordered_map<uint64_t, bool> *written = nullptr);

}  // namespace GameLogicPush
