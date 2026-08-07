#include "GameLogicPush.h"

#include "GameLogicServiceImpl.h"
#include "GatewayPushClient.h"
#include "Logging.h"

namespace GameLogicPush {

bool PushToBoundGateway(const std::string &gateway_instance_id, uint64_t player_id,
                       const std::string &session_id, const std::string &message_type,
                       const std::string &payload_or_frame, bool reliable, bool coalescable,
                       uint64_t server_seq) {
    std::string gw = gateway_instance_id;
    std::string sid = session_id;
    if (gw.empty() || sid.empty()) {
        std::string lookup_gw, lookup_sid;
        if (GameLogicGetPushTarget(player_id, &lookup_gw, &lookup_sid)) {
            if (gw.empty())
                gw = lookup_gw;
            if (sid.empty())
                sid = lookup_sid;
        }
    }
    if (gw.empty() || sid.empty()) {
        LOG_WARN << "PushToBoundGateway missing gateway/session player_id=" << player_id;
        return false;
    }
    gwpush::PushBatchRequest req;
    req.set_gateway_instance_id(gw);
    auto *m = req.add_messages();
    m->set_player_id(player_id);
    m->set_session_id(sid);
    m->set_server_seq(server_seq);
    m->set_message_type(message_type);
    m->set_payload(payload_or_frame);
    m->set_reliable(reliable);
    m->set_coalescable(coalescable);
    gwpush::PushBatchResponse rsp;
    if (!GatewayPushClient::Instance().PushBatch(gw, req, &rsp) || !rsp.ok()) {
        LOG_WARN << "PushBatch failed target_gw=" << gw
                 << " msg=" << (rsp.message().empty() ? "rpc" : rsp.message());
        return false;
    }
    if (rsp.rejected() > 0) {
        LOG_WARN << "PushBatch rejected=" << rsp.rejected() << " accepted=" << rsp.accepted()
                 << " (stale session_id?)";
    }
    return rsp.accepted() > 0;
}

}  // namespace GameLogicPush
