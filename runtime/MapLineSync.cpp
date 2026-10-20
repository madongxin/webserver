#include "MapLineSync.h"

#include "Logging.h"
#include "MapCatalog.h"
#include "MapLineView.h"
#include "PlacementStore.h"
#include "SceneKind.h"
#include "SessionStore.h"

#ifdef WEBSERVER_ENABLE_BRPC
#include "GatewayPushClient.h"
#include "gateway_push.pb.h"
#endif

#ifdef WEBSERVER_ENABLE_GAME_PROTOBUF
#include "game.pb.h"
#endif

#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace MapLineSync {
namespace {

/** 默认 8s：压测同线满员时降频分线广播，减轻 Gateway 出站与 GameLogic Push。 */
constexpr int kDefaultMinIntervalMs = 8000;

std::mutex g_mu;
std::unordered_map<uint64_t, int64_t> g_last_ms;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

uint64_t Key(uint32_t realm, uint64_t tpl) {
    return (static_cast<uint64_t>(realm) << 48) ^ tpl;
}

int MinIntervalMs() {
    if (const char *env = std::getenv("GAMEMESH_MAP_LINE_SYNC_MS")) {
        const int v = std::atoi(env);
        if (v >= 500)
            return v;
    }
    return kDefaultMinIntervalMs;
}

void OnChanged(uint32_t realm_id, uint64_t map_template_id) {
    NotifyTemplate(realm_id, map_template_id);
}

}  // namespace

void Install() {
    PlacementStore::Instance().SetLineStatusHook(&OnChanged);
}

void NotifyTemplate(uint32_t realm_id, uint64_t map_template_id) {
#if defined(WEBSERVER_ENABLE_BRPC) && defined(WEBSERVER_ENABLE_GAME_PROTOBUF)
    if (map_template_id == 0)
        return;
    MapScenePolicy pol;
    if (!MapCatalog::Instance().GetScenePolicy(map_template_id, &pol) ||
        pol.kind != SceneKind::Line)
        return;
    const uint32_t realm = MapLineView::EffectiveRealm(realm_id);
    const int64_t now = NowMs();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        int64_t &last = g_last_ms[Key(realm, map_template_id)];
        if (last != 0 && now - last < MinIntervalMs())
            return;
        last = now;
    }

    std::vector<MapLineInfo> rows;
    if (!PlacementStore::Instance().ListLines(realm, map_template_id, &rows))
        return;

    game::GameResponse inner;
    inner.set_ok(true);
    inner.set_seq(0);
    auto *qr = inner.mutable_query_map_lines();
    qr->set_map_template_id(map_template_id);
    qr->set_kind(SceneKindToString(pol.kind));
    qr->set_ok(true);
    qr->set_message("ok");
    for (const auto &r : rows)
        MapLineView::CopyLine(r, qr->add_lines());
    std::string payload;
    if (!inner.SerializeToString(&payload))
        return;

    std::vector<uint64_t> all_pids;
    all_pids.reserve(256);
    for (const auto &row : rows) {
        std::vector<uint64_t> pids;
        if (!PlacementStore::Instance().ListOccupants(row.map_instance_id, &pids) || pids.empty())
            continue;
        all_pids.insert(all_pids.end(), pids.begin(), pids.end());
    }
    if (all_pids.empty())
        return;

    std::vector<SessionStore::OnlinePushTarget> targets;
    if (!SessionStore::Instance().BatchOnlinePushTargets(all_pids, &targets) || targets.empty())
        return;

    std::map<std::string, gwpush::PushBatchRequest> batches;
    for (const auto &t : targets) {
        if (t.gateway_id.empty() || t.session_id.empty())
            continue;
        auto &preq = batches[t.gateway_id];
        preq.set_gateway_instance_id(t.gateway_id);
        auto *m = preq.add_messages();
        m->set_player_id(t.player_id);
        m->set_session_id(t.session_id);
        m->set_server_seq(0);
        m->set_message_type("map.lines.v1");
        m->set_payload(payload);
        m->set_reliable(false);
        m->set_coalescable(true);
        m->set_fence_token(t.fence_token);
        m->set_generation(t.generation);
    }
    for (auto &kv : batches) {
        if (kv.second.messages_size() == 0)
            continue;
        gwpush::PushBatchResponse prsp;
        if (!GatewayPushClient::Instance().PushBatch(kv.first, kv.second, &prsp) || !prsp.ok()) {
            LOG_WARN << "map.lines.v1 PushBatch failed gw=" << kv.first
                     << " n=" << kv.second.messages_size();
        }
    }
#else
    (void)realm_id;
    (void)map_template_id;
#endif
}

}  // namespace MapLineSync
