#include "GameLogicPush.h"

#include "AoiRecipientGate.h"
#include "GameLogicServiceImpl.h"
#include "GatewayPushClient.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PushAdmit.h"
#include "PushReplayCache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <unordered_map>

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#endif

namespace GameLogicPush {

namespace {

bool ResolveItem(GatewayPushItem *item) {
    if (!item)
        return false;
    std::string lookup_gw, lookup_sid, lookup_fence;
    uint64_t lookup_gen = 0;
    if (!GameLogicGetBoundMeta(item->player_id, &lookup_gw, &lookup_sid, &lookup_fence,
                               &lookup_gen))
        return false;
    // 地图实体上的 session 是进图时抄下来的。重连后继续用它，网关会按旧会话拒绝。
    if (!lookup_gw.empty())
        item->gateway_instance_id = lookup_gw;
    if (!lookup_sid.empty())
        item->session_id = lookup_sid;
    if (!lookup_fence.empty())
        item->fence_token = lookup_fence;
    if (lookup_gen != 0)
        item->generation = lookup_gen;
    return !item->gateway_instance_id.empty() && !item->session_id.empty();
}

void NoteReject(int reason) {
    OpsMetrics::Instance().IncPushRejected();
    if (reason > 0)
        OpsMetrics::Instance().IncReliablePushReject(reason);
}

void LogRejectSample(uint64_t player_id, int reason, uint64_t seq, int send_code) {
    const auto now = std::chrono::steady_clock::now();
    const int64_t ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    static std::atomic<int64_t> window_ms{0};
    static std::atomic<int> shown{0};
    const int64_t prev = window_ms.load(std::memory_order_relaxed);
    if (ms - prev > 1000) {
        window_ms.store(ms, std::memory_order_relaxed);
        shown.store(0, std::memory_order_relaxed);
    }
    if (shown.fetch_add(1, std::memory_order_relaxed) >= 4)
        return;
    LOG_INFO << "PushBatch item reason="
             << PushAdmitReasonName(static_cast<PushAdmitReason>(reason))
             << " player_id=" << player_id << " seq=" << seq << " send_code=" << send_code;
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
        if (!AoiRecipientGate::Instance().Allowed(item.player_id)) {
            ++dropped;
            continue;
        }
        if (!ResolveItem(&item)) {
            LOG_WARN << "PushToBoundGateway missing gateway/session player_id=" << item.player_id;
            NoteReject(static_cast<int>(PushAdmitReason::NoSession));
            LogRejectSample(item.player_id, static_cast<int>(PushAdmitReason::NoSession), 0, 0);
            AoiRecipientGate::Instance().Suppress(item.player_id);
            ++dropped;
            continue;
        }
        // 可靠序号在 Gateway 确认连接仍在之后再分配。这里先分配的话，
        // 会话还没绑上或推送被拒绝时，客户端会看到 server_seq 缺口。
        by_gw[item.gateway_instance_id].push_back(std::move(item));
    }
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
            const int reason = rsp.results_size() > 0 ? static_cast<int>(rsp.results(0).reason())
                                                       : static_cast<int>(PushAdmitReason::SendFailed);
            for (size_t i = 0; i < bucket.size(); ++i)
                NoteReject(reason == 0 ? static_cast<int>(PushAdmitReason::SendFailed) : reason);
            LOG_WARN << "PushBatch failed target_gw=" << gw << " messages=" << bucket.size()
                     << " msg=" << (rsp.message().empty() ? "rpc" : rsp.message());
            any_ok = false;
            continue;
        }
        std::array<uint32_t, 11> by_reason{};
        if (rsp.results_size() > 0) {
            const int n = std::min(rsp.results_size(), static_cast<int>(bucket.size()));
            for (int i = 0; i < n; ++i) {
                const auto &row = rsp.results(i);
                const auto &item = bucket[static_cast<size_t>(i)];
                const int reason = static_cast<int>(row.reason());
                if (row.queued()) {
                    OpsMetrics::Instance().IncPushAccepted();
                    GatewayPushItem stored = item;
                    stored.server_seq = row.server_seq();
                    if (item.reliable)
                        StoreLocalReplay(stored);
                    continue;
                }
                if (reason >= 1 && reason <= 10)
                    ++by_reason[static_cast<size_t>(reason)];
                NoteReject(reason == 0 ? static_cast<int>(PushAdmitReason::SendFailed) : reason);
                LogRejectSample(row.player_id() != 0 ? row.player_id() : item.player_id, reason,
                                row.server_seq(), 0);
                if (reason == static_cast<int>(PushAdmitReason::NoSession) ||
                    reason == static_cast<int>(PushAdmitReason::Closed))
                    AoiRecipientGate::Instance().Suppress(item.player_id);
            }
        } else if (rsp.rejected() > 0) {
            for (uint32_t i = 0; i < rsp.rejected(); ++i)
                NoteReject(static_cast<int>(PushAdmitReason::SendFailed));
            for (uint32_t i = 0; i < rsp.accepted(); ++i)
                OpsMetrics::Instance().IncPushAccepted();
        } else {
            for (uint32_t i = 0; i < rsp.accepted(); ++i)
                OpsMetrics::Instance().IncPushAccepted();
        }
        if (rsp.rejected() > 0) {
            LOG_INFO << "PushBatch rejected=" << rsp.rejected() << " accepted=" << rsp.accepted()
                     << " messages=" << bucket.size()
                     << " no_session=" << by_reason[static_cast<size_t>(PushAdmitReason::NoSession)]
                     << " player_mismatch="
                     << by_reason[static_cast<size_t>(PushAdmitReason::PlayerMismatch)]
                     << " gateway_mismatch="
                     << by_reason[static_cast<size_t>(PushAdmitReason::GatewayMismatch)]
                     << " fence_mismatch="
                     << by_reason[static_cast<size_t>(PushAdmitReason::FenceMismatch)]
                     << " generation_mismatch="
                     << by_reason[static_cast<size_t>(PushAdmitReason::GenerationMismatch)]
                     << " seq_allocation_failed="
                     << by_reason[static_cast<size_t>(PushAdmitReason::SeqAllocationFailed)]
                     << " bad_frame=" << by_reason[static_cast<size_t>(PushAdmitReason::BadFrame)]
                     << " send_failed=" << by_reason[static_cast<size_t>(PushAdmitReason::SendFailed)]
                     << " closed=" << by_reason[static_cast<size_t>(PushAdmitReason::Closed)]
                     << " replay_pending="
                     << by_reason[static_cast<size_t>(PushAdmitReason::ReplayPending)];
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
