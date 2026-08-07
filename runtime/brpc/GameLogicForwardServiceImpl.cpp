#include "GameLogicForwardServiceImpl.h"

#include "ForwardMetaContext.h"
#include "GameService.h"
#include "Logging.h"
#include "MapInstanceRegistry.h"
#include "PlayerSerialQueue.h"
#include "ProtoFraming.h"
#include "game.pb.h"

#include <brpc/controller.h>

namespace {

std::string BuildErrorFrameFromPayload(const std::string &payload, const std::string &message) {
    game::GameRequest req;
    game::GameResponse rsp;
    if (req.ParseFromString(payload))
        rsp.set_seq(req.seq());
    rsp.set_ok(false);
    rsp.set_message(message);
    std::string body;
    if (!rsp.SerializeToString(&body))
        return {};
    std::string frame;
    if (!gameproto::EncodeFrame(body, &frame))
        return {};
    return frame;
}

bool IsEnterMapPayload(const std::string &payload) {
    game::GameRequest req;
    return req.ParseFromString(payload) && req.body_case() == game::GameRequest::kEnterMap;
}

}  // namespace

void GameLogicForwardServiceImpl::Forward(::google::protobuf::RpcController *controller,
                                          const ::fwd::ForwardReq *request,
                                          ::fwd::ForwardRsp *response,
                                          ::google::protobuf::Closure *done) {
    (void)controller;
    const uint64_t player_id = request->meta().player_id();
    const std::string payload = request->request_payload();
    ForwardRouteMeta meta;
    meta.player_id = request->meta().player_id();
    meta.connection_id = request->meta().connection_id();
    meta.generation = request->meta().generation();
    meta.map_instance_id = request->meta().map_instance_id();
    meta.owner_epoch = request->meta().owner_epoch();
    meta.route_version = request->meta().route_version();
    meta.gamelogic_instance_id = request->meta().gamelogic_instance_id();

    PlayerSerialQueue::Instance().Post(player_id, [payload, response, done, player_id, meta]() {
        ForwardMetaContext::Set(meta);
        const uint64_t map_id = meta.map_instance_id;
        const uint64_t epoch = meta.owner_epoch;
        // 非进图请求：若带 map meta，必须通过本地 epoch 栅栏
        if (map_id != 0 && !IsEnterMapPayload(payload)) {
            if (!MapInstanceRegistry::Instance().AcceptWrite(map_id, epoch)) {
                LOG_WARN << "GameLogicForward: stale epoch map=" << map_id
                         << " req_epoch=" << epoch
                         << " local=" << MapInstanceRegistry::Instance().Epoch(map_id)
                         << " player_id=" << player_id;
                response->set_ok(true);
                response->set_message("stale_owner_epoch");
                response->set_response_frame(
                    BuildErrorFrameFromPayload(payload, "stale_owner_epoch"));
                ForwardMetaContext::Clear();
                done->Run();
                return;
            }
        }

        std::string out;
        if (!gameproto::HandleFrame(payload, &out)) {
            LOG_ERROR << "GameLogicForward: HandleFrame failed player_id=" << player_id;
            response->set_ok(false);
            response->set_message("handle_frame_failed");
            ForwardMetaContext::Clear();
            done->Run();
            return;
        }
        response->set_ok(true);
        response->set_response_frame(out);
        ForwardMetaContext::Clear();
        done->Run();
    });
}
