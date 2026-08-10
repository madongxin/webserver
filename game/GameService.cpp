/**
 * @file GameService.cpp
 * @brief 游戏 RPC 适配层：GameRequest <-> GameLogic <-> GameResponse
 */

#include "GameService.h"

#include "ForwardMetaContext.h"
#include "GameLogic.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "ProtoFraming.h"
#include "TrustedPlayerId.h"
#include "game.pb.h"

namespace gameproto {

bool HandleFrame(const std::string &request_payload, std::string *response_frame) {
    if (!response_frame)
        return false;

    game::GameRequest req;
    if (!req.ParseFromString(request_payload)) {
        LOG_ERROR << "GameService: ParseFromString failed";
        return false;
    }

    if (const ForwardRouteMeta *meta = ForwardMetaContext::Get()) {
        if (meta->player_id != 0) {
            const TrustedPlayerIdResult tr = ApplyTrustedPlayerId(&req, meta->player_id);
            if (tr == TrustedPlayerIdResult::Mismatch) {
                OpsMetrics::Instance().IncIdentityMismatch();
                LOG_WARN << "GameService: trusted player_id mismatch trusted=" << meta->player_id
                         << " body=" << static_cast<int>(req.body_case());
                game::GameResponse rsp;
                rsp.set_seq(req.seq());
                rsp.set_ok(false);
                rsp.set_message("ERR_PLAYER_ID_MISMATCH");
                std::string rsp_bytes;
                if (!rsp.SerializeToString(&rsp_bytes))
                    return false;
                return EncodeFrame(rsp_bytes, response_frame);
            }
        }
    }

    game::GameResponse rsp;
    GameLogic::Instance().Handle(req, &rsp);

    std::string rsp_bytes;
    if (!rsp.SerializeToString(&rsp_bytes)) {
        LOG_ERROR << "GameService: SerializeToString failed";
        return false;
    }
    return EncodeFrame(rsp_bytes, response_frame);
}

}  // namespace gameproto
