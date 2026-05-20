#include "GameService.h"

#include "GameLogic.h"
#include "Logging.h"
#include "ProtoFraming.h"
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
