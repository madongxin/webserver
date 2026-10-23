#include "GameLogicPush.h"

#include "GameLogicServiceImpl.h"
#include "GatewayPushClient.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PushReplayCache.h"

#include <chrono>
#include <unordered_map>

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#endif

namespace GameLogicPush {

namespace {

void ResolveItem(GatewayPushItem *item) {
    if (!item)
        return;
    if (!item->gateway_instance_id.empty() && !item->session_id.empty() &&
        !item->fence_token.empty())
        return;
    std::string lookup_gw, lookup_sid, lookup_fence;
    uint64_t lookup_gen = 0;
    if (!GameLogicGetBoundMeta(item->player_id, &lookup_gw, &lookup_sid, &lookup_fence,
                               &lookup_gen))
        return;
    if (item->gateway_instance_id.empty())
        item->gateway_instance_id = lookup_gw;
    if (item->session_id.empty())
        item->session_id = lookup_sid;
    if (item->fence_token.empty())
        item->fence_token = lookup_fence;
    if (item->generation == 0)
        item->generation = lookup_gen;
}

void StoreLocalReplay(const GatewayPushItem &item) {
    if (item.server_seq == 0)
        return;
#ifdef WEBSERVER_ENABLE_REDIS
    if (PushReplayStore::Instance().Available())
        return;
#endif
    PushReplayEntry e;
    e.server_seq = item.server_seq;
    e.message_type = item.message_type;
    e.payload = item.payload;
    e.reliable = true;
    PushReplayCache::Instance().Store(item.player_id, e);
}

}  // namespace

bool PushGrouped(std::vector<GatewayPushItem> items) {
    if (items.empty())
        return true;
    std::unordered_map<std::string, std::vector<GatewayPushItem>> by_gw;
    uint32_t dropped = 0;
    for (auto &item : items) {
        ResolveItem(&item);
        if (item.gateway_instance_id.empty() || item.session_id.empty()) {
            LOG_WARN << "PushToBoundGateway missing gateway/session player_id=" << item.player_id;
            ++dropped;
            continue;
        }
        // 可靠序号在 Gateway 确认连接仍在之后再分配。这里先分配的话，
        // 会话还没绑上或推送被拒绝时，客户端会看到 server_seq 缺口。
        by_gw[item.gateway_instance_id].push_back(std::move(item));
    }
    for (uint32_t i = 0; i < dropped; ++i)
        OpsMetrics::Instance().IncPushRejected();
    bool any_ok = dropped == 0;
    for (auto &kv : by_gw) {
        const std::string &gw = kv.first;
        auto &bucket = kv.second;
        gwpush::PushBatchRequest req;
        req.set_gateway_instance_id(gw);
        for (const auto &item : bucket) {
            auto *m = req.add_messages();
            m->set_player_id(item.player_id);
            m->set_session_id(item.session_id);
            m->set_server_seq(item.server_seq);
            m->set_message_type(item.message_type);
            m->set_payload(item.payload);
            m->set_reliable(item.reliable);
            m->set_coalescable(item.coalescable);
            m->set_fence_token(item.fence_token);
            m->set_generation(item.generation);
        }
        gwpush::PushBatchResponse rsp;
        const auto push_t0 = std::chrono::steady_clock::now();
        const bool pushed = GatewayPushClient::Instance().PushBatch(gw, req, &rsp);
        const auto push_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - push_t0)
                                 .count();
        OpsMetrics::Instance().NotePushBatch(push_ms);
        if (!pushed || !rsp.ok()) {
            for (size_t i = 0; i < bucket.size(); ++i)
                OpsMetrics::Instance().IncPushRejected();
            LOG_WARN << "PushBatch failed target_gw=" << gw << " messages=" << bucket.size()
                     << " msg=" << (rsp.message().empty() ? "rpc" : rsp.message());
            any_ok = false;
            continue;
        }
        if (rsp.rejected() > 0) {
            for (uint32_t i = 0; i < rsp.rejected(); ++i)
                OpsMetrics::Instance().IncPushRejected();
            LOG_INFO << "PushBatch rejected=" << rsp.rejected() << " accepted=" << rsp.accepted()
                     << " messages=" << bucket.size() << " (stale session/fence)";
        }
        for (uint32_t i = 0; i < rsp.accepted(); ++i)
            OpsMetrics::Instance().IncPushAccepted();
        if (rsp.rejected() == 0) {
            for (const auto &item : bucket) {
                if (item.reliable && rsp.accepted() > 0)
                    StoreLocalReplay(item);
            }
        }
        if (rsp.accepted() == 0)
            any_ok = false;
    }
    return any_ok && dropped == 0;
}

bool PushToBoundGateway(const std::string &gateway_instance_id, uint64_t player_id,
                       const std::string &session_id, const std::string &message_type,
                       const std::string &payload_or_frame, bool reliable, bool coalescable,
                       uint64_t server_seq, const std::string &fence_token,
                       uint64_t generation) {
    GatewayPushItem item;
    item.gateway_instance_id = gateway_instance_id;
    item.player_id = player_id;
    item.session_id = session_id;
    item.message_type = message_type;
    item.payload = payload_or_frame;
    item.reliable = reliable;
    item.coalescable = coalescable;
    item.server_seq = server_seq;
    item.fence_token = fence_token;
    item.generation = generation;
    std::vector<GatewayPushItem> items;
    items.push_back(std::move(item));
    return PushGrouped(std::move(items));
}

}  // namespace GameLogicPush
