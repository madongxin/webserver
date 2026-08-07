#include "WorldForwardServiceImpl.h"

#include "GameService.h"
#include "Logging.h"
#include "PlayerSerialQueue.h"

#include <brpc/controller.h>

void WorldForwardServiceImpl::Forward(::google::protobuf::RpcController *controller,
                                      const ::fwd::ForwardReq *request,
                                      ::fwd::ForwardRsp *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    const uint64_t player_id = request->meta().player_id();
    const std::string payload = request->request_payload();

    PlayerSerialQueue::Instance().Post(player_id, [payload, response, done, player_id]() {
        std::string out;
        if (!gameproto::HandleFrame(payload, &out)) {
            LOG_ERROR << "WorldForward: HandleFrame failed player_id=" << player_id;
            response->set_ok(false);
            response->set_message("handle_frame_failed");
            done->Run();
            return;
        }
        response->set_ok(true);
        response->set_response_frame(out);
        done->Run();
    });
}
