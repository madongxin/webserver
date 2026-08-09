#include "GameLogicPush.h"

#include "GameLogicServiceImpl.h"
#include "GatewayPushClient.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PushReplayCache.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#endif

namespace GameLogicPush {

bool PushToBoundGateway(const std::string &gateway_instance_id, uint64_t player_id,
                       const std::string &session_id, const std::string &message_type,
                       const std::string &payload_or_frame, bool reliable, bool coalescable,
                       uint64_t server_seq) {
    std::string gw = gateway_instance_id;
    std::string sid = session_id;
    std::string fence;
    uint64_t generation = 0;
    if (gw.empty() || sid.empty() || fence.empty()) {
        std::string lookup_gw, lookup_sid, lookup_fence;
        uint64_t lookup_gen = 0;
        if (GameLogicGetBoundMeta(player_id, &lookup_gw, &lookup_sid, &lookup_fence, &lookup_gen)) {
            if (gw.empty())
                gw = lookup_gw;
            if (sid.empty())
                sid = lookup_sid;
            if (fence.empty())
                fence = lookup_fence;
            if (generation == 0)
                generation = lookup_gen;
        }
    }
    if (gw.empty() || sid.empty()) {
        LOG_WARN << "PushToBoundGateway missing gateway/session player_id=" << player_id;
        return false;
    }
    uint64_t seq = server_seq;
    if (seq == 0 && reliable) {
#ifdef WEBSERVER_ENABLE_REDIS
        if (PushReplayStore::Instance().Available()) {
            seq = PushReplayStore::Instance().AppendReliable(player_id, sid, message_type,
                                                             payload_or_frame);
            if (seq == 0) {
                LOG_WARN << "PushReplayStore AppendReliable failed player=" << player_id;
                return false;
            }
        } else
#endif
        {
            seq = PushReplayCache::Instance().NextSeq(player_id);
        }
    }
    gwpush::PushBatchRequest req;
    req.set_gateway_instance_id(gw);
    auto *m = req.add_messages();
    m->set_player_id(player_id);
    m->set_session_id(sid);
    m->set_server_seq(seq);
    m->set_message_type(message_type);
    m->set_payload(payload_or_frame);
    m->set_reliable(reliable);
    m->set_coalescable(coalescable);
    m->set_fence_token(fence);
    m->set_generation(generation);
    gwpush::PushBatchResponse rsp;
    if (!GatewayPushClient::Instance().PushBatch(gw, req, &rsp) || !rsp.ok()) {
        OpsMetrics::Instance().IncPushRejected();
        LOG_WARN << "PushBatch failed target_gw=" << gw
                 << " msg=" << (rsp.message().empty() ? "rpc" : rsp.message());
        // 可靠消息已入 ReplayStore，重连仍可回放
        return false;
    }
    if (rsp.rejected() > 0) {
        OpsMetrics::Instance().IncPushRejected();
        LOG_WARN << "PushBatch rejected=" << rsp.rejected() << " accepted=" << rsp.accepted()
                 << " (stale session/fence?)";
    }
    if (rsp.accepted() > 0)
        OpsMetrics::Instance().IncPushAccepted();
    if (reliable && rsp.accepted() > 0) {
#ifdef WEBSERVER_ENABLE_REDIS
        if (!PushReplayStore::Instance().Available())
#endif
        {
            PushReplayEntry e;
            e.server_seq = seq;
            e.message_type = message_type;
            e.payload = payload_or_frame;
            e.reliable = true;
            PushReplayCache::Instance().Store(player_id, e);
        }
    }
    return rsp.accepted() > 0;
}

}  // namespace GameLogicPush
