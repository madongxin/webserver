/**
 * @file GameService.cpp
 * @brief 游戏 RPC 适配层：GameRequest <-> GameLogic <-> GameResponse
 */

#include "GameService.h"

#include "ForwardMetaContext.h"
#include "GameLogic.h"
#include "Logging.h"
#include "ProtoFraming.h"
#include "TrustedPlayerId.h"
#include "game.pb.h"

namespace gameproto {

bool HandleFrame(const std::string &request_payload, std::string *response_frame) {
    if (!response_frame)
        return false;

    // 1. 二进制 -> GameRequest（oneof body：login / consume_item / grant_item 等）
    game::GameRequest req;
    if (!req.ParseFromString(request_payload)) {
        LOG_ERROR << "GameService: ParseFromString failed";
        return false;
    }

    if (const ForwardRouteMeta *meta = ForwardMetaContext::Get()) {
        if (meta->player_id != 0)
            ApplyTrustedPlayerId(&req, meta->player_id);
    }

    // 2. 业务分发（会话校验、背包、技能 CD 等均在 GameLogic::Handle 内）
    game::GameResponse rsp;
    GameLogic::Instance().Handle(req, &rsp);

    // 3. GameResponse -> 二进制，再套上长度前缀，供网关 Send
    std::string rsp_bytes;
    if (!rsp.SerializeToString(&rsp_bytes)) {
        LOG_ERROR << "GameService: SerializeToString failed";
        return false;
    }
    return EncodeFrame(rsp_bytes, response_frame);
}

}  // namespace gameproto
