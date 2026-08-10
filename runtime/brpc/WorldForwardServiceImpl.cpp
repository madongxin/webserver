#include "WorldForwardServiceImpl.h"

#include "ForwardMetaContext.h"
#include "GameService.h"
#include "Logging.h"
#include "PlayerSerialQueue.h"

#include <brpc/controller.h>

void WorldForwardServiceImpl::Forward(::google::protobuf::RpcController *controller,
                                      const ::fwd::ForwardReq *request,
                                      ::fwd::ForwardRsp *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request) {
        response->set_ok(false);
        response->set_message("null request");
        return;
    }
    const uint64_t player_id = request->meta().player_id();
    const std::string payload = request->request_payload();
    ForwardRouteMeta meta;
    meta.player_id = player_id;
    meta.connection_id = request->meta().connection_id();
    meta.generation = request->meta().generation();
    meta.map_instance_id = request->meta().map_instance_id();
    meta.owner_epoch = request->meta().owner_epoch();
    meta.route_version = request->meta().route_version();
    meta.gamelogic_instance_id = request->meta().gamelogic_instance_id();
    meta.session_id = request->meta().session_id();
    meta.fence_token = request->meta().fence_token();

    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    if (!PlayerSerialQueue::Instance().TryPost(player_id, [payload, meta, rsp, closure, player_id]() {
            brpc::ClosureGuard g(closure);
            ForwardMetaContext::Set(meta);
            std::string out;
            const bool ok = gameproto::HandleFrame(payload, &out);
            ForwardMetaContext::Clear();
            if (!ok) {
                LOG_ERROR << "WorldForward: HandleFrame failed player_id=" << player_id;
                rsp->set_ok(false);
                rsp->set_message("handle_frame_failed");
                return;
            }
            rsp->set_ok(true);
            rsp->set_response_frame(out);
        })) {
        brpc::ClosureGuard g(closure);
        rsp->set_ok(false);
        rsp->set_message("ERR_OVERLOAD");
    }
}
