#include "GameLogic.h"

#include "AoiPublishQueue.h"
#include "FormalMode.h"
#include "ForwardMetaContext.h"
#include "FriendService.h"
#include "LogicMetrics.h"
#include "PacketTiming.h"
#include "MapCatalog.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#include "MapRuntime.h"
#include "PublicError.h"
#include "SceneKind.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "MapLineView.h"
#include "PlacementAuthority.h"
#include "PlacementStore.h"
#include "PushReplayStore.h"
#include "SessionStore.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "AoiRecipientGate.h"
#include "BrpcGameDbRepository.h"
#include "GameLogicPush.h"
#include "GameLogicServiceImpl.h"
#include "GatewayPushClient.h"
#include "OpsMetrics.h"
#include "RpcOffloadPool.h"
#include "SessionRpcClient.h"
#include "session.pb.h"
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "MailService.h"
#include "PlayerAccountStore.h"
#include "PlayerItemPersistQueue.h"
#include "PlayerItemStore.h"
#include "PlayerProfileStore.h"
#endif

#include "Logging.h"
#include "SocialText.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <memory>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <unordered_set>
#include <utility>
#include <vector>

static int EnvBoundedInt(const char *k, int def, int lo, int hi) {
    if (!k)
        return def;
    const char *e = std::getenv(k);
    if (!e || !*e)
        return def;
    const int v = std::atoi(e);
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 1002 进图出生点用客户端面板上的坐标。不改地图文件，避免地形哈希和客户端对不上。
void ApplyTemplateSpawn(uint64_t map_template_id, float *x, float *y, float *z) {
    if (map_template_id != 1002 || !x || !y || !z)
        return;
    *x = 179.30f;
    *y = -18.62f;
    *z = 324.20f;
}

#ifdef WEBSERVER_ENABLE_REDIS
bool FetchAuthorityPlacement(uint64_t map_instance_id, PlacementRecord *out, std::string *err) {
    if (!out || map_instance_id == 0) {
        if (err)
            *err = "ERR_INVALID_MAP";
        return false;
    }
    if (PlacementStore::Instance().Available() &&
        PlacementStore::Instance().Get(map_instance_id, out)) {
        if (out->state == PlacementState::Closed) {
            MapRuntime::Instance().Unload(map_instance_id);
            if (err)
                *err = IsDungeonKind(out->kind) ? "ERR_DUNGEON_NOT_FOUND" : "ERR_MAP_NOT_READY";
            return false;
        }
        return true;
    }
#ifdef WEBSERVER_ENABLE_BRPC
    if (SessionRpcClient::Instance().ready()) {
        sess::GetPlacementResponse grsp;
        if (SessionRpcClient::Instance().GetPlacement(map_instance_id, &grsp) && grsp.ok() &&
            grsp.has_placement()) {
            const auto &p = grsp.placement();
            out->realm_id = p.realm_id();
            out->map_template_id = p.map_template_id();
            out->map_instance_id = p.map_instance_id();
            out->owner_logic_server_id = p.owner_logic_server_id();
            out->owner_epoch = p.owner_epoch();
            out->route_version = p.route_version();
            out->state = PlacementStore::StateFromString(p.state());
            out->updated_at = p.updated_at();
            out->lease_until = p.lease_until();
            out->kind = p.kind();
            if (out->state == PlacementState::Closed) {
                MapRuntime::Instance().Unload(map_instance_id);
                if (err)
                    *err = IsDungeonKind(out->kind) ? "ERR_DUNGEON_NOT_FOUND" : "ERR_MAP_NOT_READY";
                return false;
            }
            return true;
        }
    }
#endif
    if (err)
        *err = "ERR_PLACEMENT_NOT_FOUND";
    return false;
}

bool ValidateLocalAuthorityWrite(const PlacementRecord &auth, uint64_t req_epoch,
                                 uint64_t req_route_ver, std::string *err_code,
                                 bool require_complete_fence) {
    return ValidateAuthorityWrite(auth, req_epoch, req_route_ver,
                                  MapInstanceRegistry::Instance().local_instance_id(), err_code, 0,
                                  require_complete_fence);
}
#endif

void FillEntitySnapshot(const MapEntity &e, game::EntitySnapshot *out) {
    if (!out)
        return;
    out->set_player_id(e.player_id);
    out->set_player_name(e.player_name);
    out->mutable_position()->set_x(e.x);
    out->mutable_position()->set_y(e.y);
    out->mutable_position()->set_z(e.z);
    out->set_yaw(e.yaw);
    out->set_hp(e.hp);
    out->set_max_hp(e.max_hp);
    out->set_state_seq(e.state_seq);
}

void FillMoveSnapshot(const MapEntity &e, game::EntitySnapshot *out) {
    if (!out)
        return;
    out->set_player_id(e.player_id);
    out->mutable_position()->set_x(e.x);
    out->mutable_position()->set_y(e.y);
    out->mutable_position()->set_z(e.z);
    out->set_yaw(e.yaw);
    out->set_state_seq(e.state_seq);
}

#ifdef WEBSERVER_ENABLE_BRPC
std::mutex g_aoi_mu;
std::unordered_set<uint64_t> g_aoi_held;
std::unordered_map<uint64_t, AoiPushBatch> g_aoi_held_batch;
struct AoiLane {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<AoiPushBatch> q;
    std::thread worker;
};
std::vector<std::unique_ptr<AoiLane>> g_aoi_lanes;
std::once_flag g_aoi_start_once;
std::atomic<bool> g_aoi_stop{false};
std::atomic<int64_t> g_aoi_queued{0};
constexpr size_t kAoiQueueCap = 64;
constexpr int64_t kAoiLaneHoldMs = 150;

void AoiLaneMain(AoiLane *lane);

void PublishAoiBatch(const AoiPushBatch &batch);

// 积压时对同一收件人+实体的 MOVE 按约 100ms 抽样（≥10 次/秒），保留递增 state_seq。
// 100ms 内的密采样只留最新；跨窗的样本都发出，避免长时间合并成一次大跳。
// ENTER/LEAVE 原样保留，并打断该实体的 MOVE 合并窗口。
// 600 同线压测：100ms 仍易打满出站；200ms 合并 Move，ENTER/LEAVE 仍原样保留。
constexpr int64_t kAoiMoveSampleMs = 200;

void SampleMoveEvents(std::vector<AoiRecipientEvent> *events) {
    if (!events || events->size() < 2)
        return;
    std::vector<AoiRecipientEvent> out;
    out.reserve(events->size());
    // recipient -> entity -> {out_index, last_kept_server_ms}
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::pair<size_t, int64_t>>> move_at;
    for (auto &ev : *events) {
        const uint64_t ent = ev.snapshot.player_id;
        if (ev.op == 2) {
            const int64_t t = ev.snapshot.last_move_server_ms;
            auto rit = move_at.find(ev.recipient_id);
            if (rit != move_at.end()) {
                auto eit = rit->second.find(ent);
                if (eit != rit->second.end()) {
                    const int64_t prev_t = eit->second.second;
                    if (t > 0 && prev_t > 0 && (t - prev_t) < kAoiMoveSampleMs) {
                        // state_seq 变小的帧客户端会整条丢掉，人就停在上一帧。
                        if (ev.snapshot.state_seq >= out[eit->second.first].snapshot.state_seq) {
                            out[eit->second.first] = std::move(ev);
                            eit->second.second = t;
                        }
                        continue;
                    }
                }
            }
            move_at[ev.recipient_id][ent] = {out.size(), t};
            out.push_back(std::move(ev));
        } else {
            auto rit = move_at.find(ev.recipient_id);
            if (rit != move_at.end())
                rit->second.erase(ent);
            out.push_back(std::move(ev));
        }
    }
    *events = std::move(out);
}

void PublishTaken(std::vector<AoiPushBatch> taken) {
    if (taken.empty())
        return;
    if (taken.size() == 1) {
        PublishAoiBatch(taken[0]);
        return;
    }
    std::unordered_map<uint64_t, AoiPushBatch> merged;
    for (auto &b : taken) {
        auto &dst = merged[b.map_instance_id];
        dst.map_instance_id = b.map_instance_id;
        dst.events.insert(dst.events.end(), std::make_move_iterator(b.events.begin()),
                          std::make_move_iterator(b.events.end()));
    }
    for (auto &kv : merged)
        PublishAoiBatch(kv.second);
}

void AoiLaneMain(AoiLane *lane) {
    for (;;) {
        std::vector<AoiPushBatch> taken;
        {
            std::unique_lock<std::mutex> lk(lane->mu);
            lane->cv.wait(lk, [&] {
                return g_aoi_stop.load(std::memory_order_relaxed) || !lane->q.empty();
            });
            if (lane->q.empty())
                return;
            const int64_t queued_at = lane->q.front().enqueued_ms;
            const bool stop = g_aoi_stop.load(std::memory_order_relaxed);
            bool reliable = false;
            for (const auto &b : lane->q) {
                if (!AoiBatchIsMoveOnly(b)) {
                    reliable = true;
                    break;
                }
            }
            if (!stop && !reliable && queued_at > 0) {
                const auto ready = std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(queued_at + kAoiLaneHoldMs));
                lane->cv.wait_until(lk, ready, [&] {
                    if (g_aoi_stop.load(std::memory_order_relaxed) || lane->q.empty())
                        return true;
                    for (const auto &b : lane->q) {
                        if (!AoiBatchIsMoveOnly(b))
                            return true;
                    }
                    return NowMs() >= queued_at + kAoiLaneHoldMs;
                });
            }
            if (lane->q.empty())
                return;
            const int64_t oldest =
                lane->q.front().enqueued_ms > 0 ? NowMs() - lane->q.front().enqueued_ms : 0;
            taken.reserve(lane->q.size());
            while (!lane->q.empty()) {
                taken.push_back(std::move(lane->q.front()));
                lane->q.pop_front();
            }
            g_aoi_queued.fetch_sub(static_cast<int64_t>(taken.size()), std::memory_order_relaxed);
            OpsMetrics::Instance().SetAoiPushBacklog(g_aoi_queued.load(std::memory_order_relaxed));
            OpsMetrics::Instance().SetAoiWorkerInflight(static_cast<int64_t>(taken.size()));
            OpsMetrics::Instance().SetAoiBatchOldestAgeMs(oldest);
        }
        PublishTaken(std::move(taken));
        OpsMetrics::Instance().SetAoiWorkerInflight(0);
        OpsMetrics::Instance().SetAoiBatchOldestAgeMs(0);
    }
}

void EnsureAoiWorker() {
    std::call_once(g_aoi_start_once, [] {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc < 4)
            hc = 4;
        g_aoi_lanes.reserve(hc);
        for (unsigned i = 0; i < hc; ++i)
            g_aoi_lanes.push_back(std::unique_ptr<AoiLane>(new AoiLane()));
        for (auto &lane : g_aoi_lanes)
            lane->worker = std::thread(AoiLaneMain, lane.get());
        OpsMetrics::Instance().SetAoiWorkerThreads(static_cast<int64_t>(g_aoi_lanes.size()));
    });
}

void EnqueueAoi(AoiPushBatch batch) {
    if (batch.events.empty())
        return;
    if (g_aoi_stop.load(std::memory_order_acquire))
        return;
    EnsureAoiWorker();
    if (g_aoi_lanes.empty())
        return;
    const size_t lane_i =
        static_cast<size_t>(batch.map_instance_id % static_cast<uint64_t>(g_aoi_lanes.size()));
    AoiLane *lane = g_aoi_lanes[lane_i].get();
    {
        std::lock_guard<std::mutex> lk(lane->mu);
        batch.enqueued_ms = NowMs();
        const size_t before = lane->q.size();
        lane->q.push_back(std::move(batch));
        const size_t shed = ShedAoiMoveBatches(&lane->q, kAoiQueueCap);
        if (shed != 0)
            OpsMetrics::Instance().AddAoiMoveShed(static_cast<uint64_t>(shed));
        const size_t after = lane->q.size();
        g_aoi_queued.fetch_add(static_cast<int64_t>(after) - static_cast<int64_t>(before),
                               std::memory_order_relaxed);
        OpsMetrics::Instance().SetAoiPushBacklog(g_aoi_queued.load(std::memory_order_relaxed));
    }
    lane->cv.notify_one();
}

void StopAoiWorker() {
    g_aoi_stop.store(true, std::memory_order_release);
    for (auto &lane : g_aoi_lanes)
        lane->cv.notify_all();
    for (auto &lane : g_aoi_lanes) {
        if (lane->worker.joinable())
            lane->worker.join();
    }
}

void PublishAoiBatch(const AoiPushBatch &batch) {
    if (batch.events.empty())
        return;
    AoiPushBatch live;
    live.map_instance_id = batch.map_instance_id;
    {
        std::lock_guard<std::mutex> lk(g_aoi_mu);
        for (const auto &ev : batch.events) {
            if (g_aoi_held.count(ev.recipient_id) != 0) {
                auto &held = g_aoi_held_batch[ev.recipient_id];
                held.map_instance_id = batch.map_instance_id;
                held.events.push_back(ev);
            } else {
                live.events.push_back(ev);
            }
        }
    }
    if (live.events.empty())
        return;
    SampleMoveEvents(&live.events);
    const int64_t emit_ms = NowMs();
    std::unordered_map<uint64_t, game::AoiDelta> by_player;
    std::unordered_map<uint64_t, std::pair<std::string, std::string>> route;
    for (const auto &ev : live.events) {
        if (!AoiRecipientGate::Instance().Allowed(ev.recipient_id))
            continue;
        auto &delta = by_player[ev.recipient_id];
        delta.set_map_instance_id(live.map_instance_id);
        auto *pe = delta.add_events();
        pe->set_op(ev.op);
        if (ev.op == 2)
            FillMoveSnapshot(ev.snapshot, pe->mutable_entity());
        else
            FillEntitySnapshot(ev.snapshot, pe->mutable_entity());
        const int64_t t =
            ev.snapshot.last_move_server_ms > 0 ? ev.snapshot.last_move_server_ms : emit_ms;
        pe->set_server_time_ms(t);
        if (route[ev.recipient_id].first.empty())
            route[ev.recipient_id] = {ev.gateway_instance_id, ev.session_id};
    }
    std::vector<GameLogicPush::GatewayPushItem> items;
    items.reserve(by_player.size());
    for (auto &kv : by_player) {
        bool has_el = false;
        bool has_move = false;
        for (const auto &e : kv.second.events()) {
            if (e.op() == 2)
                has_move = true;
            else
                has_el = true;
        }
        game::GameResponse notify;
        notify.set_ok(true);
        notify.set_message("aoi");
        *notify.mutable_aoi_delta() = kv.second;
        GameLogicPush::GatewayPushItem item;
        if (!notify.SerializeToString(&item.payload)) {
            LOG_WARN << "PublishAoiBatch serialize failed player=" << kv.first;
            continue;
        }
        const auto &rt = route[kv.first];
        item.gateway_instance_id = rt.first;
        item.player_id = kv.first;
        item.session_id = rt.second;
        item.message_type = "aoi.delta.v1";
        // 纯移动不占可靠序号。只有进入、离开或其它状态变更才写入回放列表。
        item.reliable = has_el;
        item.coalescable = has_move && !has_el;
        items.push_back(std::move(item));
    }
    if (!items.empty())
        GameLogicPush::PushGrouped(std::move(items));
}
#endif

}  // namespace

GameLogic &GameLogic::Instance() {
    static GameLogic g;
    return g;
}

void GameLogic::EmitAoi(const AoiPushBatch &batch) {
#ifdef WEBSERVER_ENABLE_BRPC
    EnqueueAoi(batch);
#else
    (void)batch;
#endif
}

void GameLogic::StopAoiPublisher() {
#ifdef WEBSERVER_ENABLE_BRPC
    StopAoiWorker();
#endif
}

void GameLogic::SetAoiHold(uint64_t player_id, bool hold) {
#ifdef WEBSERVER_ENABLE_BRPC
    if (player_id == 0)
        return;
    AoiPushBatch flushed;
    {
        std::lock_guard<std::mutex> lk(g_aoi_mu);
        if (hold) {
            g_aoi_held.insert(player_id);
            return;
        }
        g_aoi_held.erase(player_id);
        auto it = g_aoi_held_batch.find(player_id);
        if (it == g_aoi_held_batch.end())
            return;
        flushed = std::move(it->second);
        g_aoi_held_batch.erase(it);
    }
    EnqueueAoi(std::move(flushed));
#else
    (void)player_id;
    (void)hold;
#endif
}

bool GameLogic::EnsurePlayerLoaded(uint64_t player_id, std::string *err) {
    // 已有内存态（含 Import/迁移）视为已加载
    if (inventory_.count(player_id) != 0) {
        player_load_ok_[player_id] = true;
        if (skill_cd_until_ms_.count(player_id) == 0)
            skill_cd_until_ms_[player_id] = {};
        return true;
    }
    if (player_load_ok_.count(player_id) && !player_load_ok_[player_id]) {
        if (err)
            *err = "player load previously failed";
        return false;
    }
#if defined(WEBSERVER_ENABLE_BRPC)
    if (FormalModeEnabled()) {
        if (!BrpcGameDbRepository::Instance().started()) {
            player_load_ok_[player_id] = false;
            if (err)
                *err = "gamedb unavailable";
            LOG_WARN << "EnsurePlayerLoaded GameDB not started player_id=" << player_id;
            return false;
        }
        std::map<uint32_t, uint32_t> loaded;
        uint64_t ver = 1;
        std::string load_err;
        if (!BrpcGameDbRepository::Instance().LoadInventory(player_id, &loaded, &ver, &load_err)) {
            player_load_ok_[player_id] = false;
            if (err)
                *err = load_err.empty() ? "gamedb load failed" : load_err;
            LOG_WARN << "EnsurePlayerLoaded GameDB fail player_id=" << player_id
                     << " err=" << (err ? *err : load_err);
            return false;
        }
        inventory_[player_id] = std::move(loaded);
        asset_version_[player_id] = ver == 0 ? 1 : ver;
        skill_cd_until_ms_[player_id] = {};
        player_load_ok_[player_id] = true;
        LOG_INFO << "EnsurePlayerLoaded from GameDB player_id=" << player_id
                 << " kinds=" << inventory_[player_id].size() << " ver=" << asset_version_[player_id];
        return true;
    }
#endif
    auto &inv = inventory_[player_id];
#ifdef WEBSERVER_ENABLE_MYSQL
    std::map<uint32_t, uint32_t> loaded;
    if (PlayerItemStore::Instance().LoadInventoryAggregate(player_id, &loaded) && !loaded.empty()) {
        inv = std::move(loaded);
        LOG_INFO << "EnsurePlayer loaded bag from DB player_id=" << player_id
                 << " kinds=" << inv.size();
    } else {
        inv.clear();
    }
#else
    inv[1001] = 10;
    inv[1002] = 5;
#endif
    if (skill_cd_until_ms_.count(player_id) == 0)
        skill_cd_until_ms_[player_id] = {};
    if (asset_version_.count(player_id) == 0)
        asset_version_[player_id] = 1;
    player_load_ok_[player_id] = true;
    return true;
}

void GameLogic::EnsurePlayer(uint64_t player_id) {
    std::string err;
    (void)EnsurePlayerLoaded(player_id, &err);
    // 非 Formal 兼容：保证 key 存在，避免调用方 operator[] 歧义
    if (inventory_.count(player_id) == 0 && !FormalModeEnabled()) {
        inventory_[player_id] = {};
        skill_cd_until_ms_[player_id] = {};
        asset_version_[player_id] = 1;
        player_load_ok_[player_id] = true;
    }
}

uint32_t GameLogic::GetItemCount(uint64_t player_id, uint32_t item_id) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    return inventory_[player_id][item_id];
}

void GameLogic::CopyInventory(uint64_t player_id, std::unordered_map<uint32_t, uint32_t> *out) {
    if (!out)
        return;
    out->clear();
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    for (const auto &kv : inventory_[player_id])
        (*out)[kv.first] = kv.second;
}

bool GameLogic::ApplyItemReward(uint64_t player_id, uint32_t item_id, uint32_t count) {
    if (count == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    inventory_[player_id][item_id] += count;
    return true;
}

bool GameLogic::LoadAssetsFromGameDbUnlocked(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                                             uint64_t *ver) {
    if (!bag || !ver)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (reload_blocked_for_test_)
            return false;
        if (reload_override_for_test_) {
            *bag = reload_override_bag_;
            *ver = reload_override_ver_;
            return true;
        }
    }
    bag->clear();
    *ver = 0;
#if defined(WEBSERVER_ENABLE_BRPC)
    if (FormalModeEnabled() && BrpcGameDbRepository::Instance().started()) {
        std::string load_err;
        if (!BrpcGameDbRepository::Instance().LoadInventory(player_id, bag, ver, &load_err)) {
            LOG_WARN << "LoadAssetsFromGameDb brpc fail player=" << player_id
                     << " err=" << load_err;
            return false;
        }
        if (*ver == 0)
            *ver = 1;
        return true;
    }
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
    if (GameDbAssetStore::Instance().Available() || GameDbAssetStore::Instance().EnsureTables()) {
        if (!GameDbAssetStore::Instance().LoadInventory(player_id, bag, ver)) {
            LOG_WARN << "LoadAssetsFromGameDb mysql fail player=" << player_id;
            return false;
        }
        if (*ver == 0)
            *ver = 1;
        return true;
    }
#endif
    (void)player_id;
    return false;
}

bool GameLogic::TryReloadAssets(uint64_t player_id) {
    std::map<uint32_t, uint32_t> bag;
    uint64_t ver = 0;
    if (!LoadAssetsFromGameDbUnlocked(player_id, &bag, &ver))
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    inventory_[player_id] = std::move(bag);
    asset_version_[player_id] = ver;
    asset_dirty_.erase(player_id);
    player_load_ok_[player_id] = true;
    return true;
}

bool GameLogic::IsAssetDirty(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    return asset_dirty_.count(player_id) != 0 && asset_dirty_[player_id];
}

bool GameLogic::EnsureAssetsSynced(uint64_t player_id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = asset_dirty_.find(player_id);
        if (it == asset_dirty_.end() || !it->second)
            return true;
    }
    if (TryReloadAssets(player_id))
        return true;
    LOG_WARN << "EnsureAssetsSynced failed player=" << player_id << " STATE_SYNC_REQUIRED";
    return false;
}

bool GameLogic::ReloadAuthoritativeAssets(uint64_t player_id) {
    if (TryReloadAssets(player_id))
        return true;
    std::lock_guard<std::mutex> lk(mu_);
    asset_dirty_[player_id] = true;
    return false;
}

void GameLogic::SetAssetReloadBlockedForTest(bool v) {
    std::lock_guard<std::mutex> lk(mu_);
    reload_blocked_for_test_ = v;
}

void GameLogic::SetAssetApplyBlockedForTest(bool v) {
    std::lock_guard<std::mutex> lk(mu_);
    apply_blocked_for_test_ = v;
}

void GameLogic::SetAssetReloadOverrideForTest(const std::map<uint32_t, uint32_t> &bag, uint64_t ver) {
    std::lock_guard<std::mutex> lk(mu_);
    reload_override_for_test_ = true;
    reload_override_bag_ = bag;
    reload_override_ver_ = ver;
}

void GameLogic::ClearAssetReloadOverrideForTest() {
    std::lock_guard<std::mutex> lk(mu_);
    reload_override_for_test_ = false;
    reload_override_bag_.clear();
    reload_override_ver_ = 0;
}

bool GameLogic::ApplyItemRewardsWithVersion(uint64_t player_id,
                                            const std::vector<GameDbGrantedItem> &grants,
                                            uint64_t committed_asset_version) {
    if (player_id == 0 || committed_asset_version == 0)
        return false;
    bool need_reload = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        EnsurePlayer(player_id);
        const uint64_t cur = asset_version_[player_id];
        const bool dirty = asset_dirty_.count(player_id) != 0 && asset_dirty_[player_id];
        if (dirty) {
            need_reload = true;
        } else if (cur == committed_asset_version) {
            return true;
        } else if (cur > committed_asset_version) {
            return true;
        } else if (!apply_blocked_for_test_ && committed_asset_version == cur + 1 &&
                   !grants.empty()) {
            std::map<uint32_t, uint32_t> next = inventory_[player_id];
            bool overflow = false;
            for (const auto &g : grants) {
                if (g.count == 0)
                    continue;
                if (g.asset_id > 0xFFFFFFFFu) {
                    overflow = true;
                    break;
                }
                const uint32_t item_id = static_cast<uint32_t>(g.asset_id);
                const uint64_t sum = static_cast<uint64_t>(next[item_id]) + g.count;
                if (sum > 0xFFFFFFFFu) {
                    overflow = true;
                    break;
                }
                next[item_id] = static_cast<uint32_t>(sum);
            }
            if (!overflow) {
                inventory_[player_id] = std::move(next);
                asset_version_[player_id] = committed_asset_version;
                asset_dirty_.erase(player_id);
                return true;
            }
            need_reload = true;
            LOG_WARN << "ApplyItemRewardsWithVersion overflow player=" << player_id
                     << " -> reload";
        } else {
            need_reload = true;
            LOG_WARN << "ApplyItemRewardsWithVersion stale/gap player=" << player_id << " mem=" << cur
                     << " committed=" << committed_asset_version << " apply_blocked="
                     << (apply_blocked_for_test_ ? 1 : 0) << " -> reload";
        }
    }
    (void)need_reload;
    if (!TryReloadAssets(player_id)) {
        std::lock_guard<std::mutex> lk(mu_);
        asset_dirty_[player_id] = true;
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (asset_version_[player_id] >= committed_asset_version) {
        asset_dirty_.erase(player_id);
        return true;
    }
    asset_dirty_[player_id] = true;
    return false;
}

uint64_t GameLogic::GetAssetVersion(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    return asset_version_[player_id];
}

bool GameLogic::RollbackItemReward(uint64_t player_id, uint32_t item_id, uint32_t count) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    auto &n = inventory_[player_id][item_id];
    if (n < count)
        n = 0;
    else
        n -= count;
    ++asset_version_[player_id];
    return true;
}

bool GameLogic::ExportRuntimeState(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                                   std::map<uint32_t, int64_t> *skill_cds, uint64_t *asset_version) {
    if (!bag || !skill_cds)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    *bag = inventory_[player_id];
    *skill_cds = skill_cd_until_ms_[player_id];
    if (asset_version)
        *asset_version = asset_version_[player_id];
    return true;
}

bool GameLogic::ImportRuntimeState(uint64_t player_id, const std::map<uint32_t, uint32_t> &bag,
                                   const std::map<uint32_t, int64_t> &skill_cds,
                                   uint64_t asset_version) {
    if (player_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    inventory_[player_id] = bag;
    skill_cd_until_ms_[player_id] = skill_cds;
    asset_version_[player_id] = asset_version == 0 ? 1 : asset_version;
    player_load_ok_[player_id] = true;
    asset_dirty_.erase(player_id);
    return true;
}

bool GameLogic::BuildFullStateSnapshot(uint64_t player_id, game::FullStateSnapshotRsp *out) {
    return BuildFullStateSnapshot(player_id, 0, out);
}

std::string GameLogic::LifeStateOf(uint64_t player_id) {
    auto it = profiles_.find(player_id);
    if (it == profiles_.end() || it->second.life_state().empty())
        return "ALIVE";
    return it->second.life_state();
}

bool GameLogic::BuildFullStateSnapshot(uint64_t player_id, uint64_t baseline_server_seq,
                                       game::FullStateSnapshotRsp *out) {
    if (!out || player_id == 0)
        return false;
    if (IsAssetDirty(player_id) && !TryReloadAssets(player_id)) {
        out->Clear();
        out->set_ok(false);
        out->set_player_id(player_id);
        out->set_error_code("STATE_SYNC_REQUIRED");
        out->set_message("STATE_SYNC_REQUIRED");
        return false;
    }
    out->Clear();
    game::PlayerAttributes attrs;
    const bool have_profile = GetPlayerAttributes(player_id, &attrs);
    std::unique_lock<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    out->set_player_id(player_id);
    out->set_asset_version(asset_version_[player_id]);
    for (const auto &kv : inventory_[player_id]) {
        if (kv.second == 0)
            continue;
        out->add_item_ids(kv.first);
        out->add_item_counts(kv.second);
    }
    if (have_profile)
        *out->mutable_profile() = attrs;
    else if (profiles_.count(player_id))
        *out->mutable_profile() = profiles_[player_id];
    out->set_life_state(LifeStateOf(player_id));
    out->set_snapshot_version(1);
    out->set_baseline_server_seq(baseline_server_seq);
    out->set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());

    // last_applied_server_seq 只记在 baseline 上，不能拿来裁 aoi_entities。
    // 客户端已经应用到这个 seq 时，增量是空的，全量视野不能跟着变成空列表。
    MapRuntime::FullView view;
    MapRuntime::Instance().CollectFullView(player_id, &view);
    if (!view.ready && std::string(view.step) == "viewer_not_online") {
        lk.unlock();
        std::string gw, sid;
#ifdef WEBSERVER_ENABLE_BRPC
        GameLogicGetBoundMeta(player_id, &gw, &sid, nullptr, nullptr);
#endif
        AoiPushBatch pushes;
        MapEntity self;
        std::vector<MapEntity> snap;
        if (MapRuntime::Instance().Reconnect(player_id, gw, sid, &self, &snap, &pushes))
            EmitAoi(pushes);
        MapRuntime::Instance().CollectFullView(player_id, &view);
        lk.lock();
    }
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementRecord live_place;
    bool have_live_place = false;
    if (!view.ready && std::string(view.step) == "not_on_map") {
        lk.unlock();
        if (PlacementStore::Instance().Available()) {
            uint64_t presence_mid = 0;
            if (PlacementStore::Instance().GetPlayerPresence(player_id, &presence_mid) &&
                presence_mid != 0 && PlacementStore::Instance().Get(presence_mid, &live_place) &&
                live_place.state == PlacementState::Ready && live_place.map_instance_id != 0)
                have_live_place = true;
        }
        lk.lock();
    }
#endif
    // 与 MapRuntime 可见上限 200 对齐。128 会在密集视野里直接 ERR_SNAPSHOT_TOO_LARGE，恢复不了周围的人。
    uint32_t aoi_max = 200;
    if (const char *e = std::getenv("GAMEMESH_SNAPSHOT_AOI_MAX")) {
        const int v = std::atoi(e);
        if (v > 0 && v <= 4096)
            aoi_max = static_cast<uint32_t>(v);
    }
    if (view.ready) {
        if (view.interest.size() > aoi_max) {
            out->set_ok(false);
            out->set_error_code("ERR_SNAPSHOT_TOO_LARGE");
            out->set_message("ERR_SNAPSHOT_TOO_LARGE");
            out->set_map_instance_id(view.map_instance_id);
            LOG_INFO << "full snapshot player=" << player_id << " map=" << view.map_instance_id
                     << " radius_m=" << view.radius_m << " radius_cells=" << view.radius_cells
                     << " interest=" << view.interest.size() << " aoi=0"
                     << " ver=" << out->snapshot_version() << " baseline=" << baseline_server_seq
                     << " step=too_large cache=" << view.cache_n;
            return false;
        }
        FillEntitySnapshot(view.self, out->mutable_self());
        for (const auto &e : view.interest) {
            if (e.player_id == player_id)
                continue;
            FillEntitySnapshot(e, out->add_aoi_entities());
        }
        out->set_map_instance_id(view.map_instance_id);
        LOG_INFO << "full snapshot player=" << player_id << " map=" << view.map_instance_id
                 << " radius_m=" << view.radius_m << " radius_cells=" << view.radius_cells
                 << " interest=" << view.interest.size() << " aoi=" << out->aoi_entities_size()
                 << " ver=" << out->snapshot_version() << " baseline=" << baseline_server_seq
                 << " step=" << view.step << " cache=" << view.cache_n;
        out->set_map_template_id(MapInstanceRegistry::Instance().TemplateId(view.map_instance_id));
        out->set_owner_epoch(MapInstanceRegistry::Instance().Epoch(view.map_instance_id));
        auto sit = last_safe_.find(player_id);
        if (sit != last_safe_.end()) {
            out->set_realm_id(sit->second.realm_id);
            if (out->map_template_id() == 0)
                out->set_map_template_id(sit->second.map_template_id);
            out->set_route_version(0);
        }
#ifdef WEBSERVER_ENABLE_BRPC
        if (const ForwardRouteMeta *m = ForwardMetaContext::Get()) {
            if (m->route_version != 0)
                out->set_route_version(m->route_version);
            if (m->owner_epoch != 0)
                out->set_owner_epoch(m->owner_epoch);
        }
#endif
        out->set_recovery_reason("INSTANCE_LIVE");
    } else if (std::string(view.step) != "not_on_map") {
        out->set_ok(false);
        out->set_error_code(gameproto::kErrAoiResyncRequired);
        out->set_message("AOI interest not ready");
        out->set_player_id(player_id);
        if (view.map_instance_id != 0)
            out->set_map_instance_id(view.map_instance_id);
        LOG_INFO << "full snapshot player=" << player_id << " map=" << view.map_instance_id
                 << " radius_m=" << view.radius_m << " radius_cells=" << view.radius_cells
                 << " interest=0 aoi=0 ver=" << out->snapshot_version()
                 << " baseline=" << baseline_server_seq << " step=" << view.step
                 << " cache=" << view.cache_n;
        return false;
    } else {
        out->set_recovery_reason("NOT_ON_MAP");
        auto sit = last_safe_.find(player_id);
        if (sit != last_safe_.end() && sit->second.map_template_id != 0) {
            out->set_realm_id(sit->second.realm_id);
            out->set_map_template_id(sit->second.map_template_id);
            out->mutable_self()->set_player_id(player_id);
            out->mutable_self()->mutable_position()->set_x(sit->second.x);
            out->mutable_self()->mutable_position()->set_y(sit->second.y);
            out->mutable_self()->mutable_position()->set_z(sit->second.z);
            out->mutable_self()->set_yaw(sit->second.yaw);
            out->set_recovery_reason("REENTER_FROM_SAFE_POS");
            if (sit->second.map_instance_id != 0)
                out->set_map_instance_id(sit->second.map_instance_id);
        }
#ifdef WEBSERVER_ENABLE_REDIS
        // 人已经在线上时，本逻辑的 MapRuntime 可能还没有他（登录绑到了另一台 GameLogic）。
        // 快照不带真实 map_instance_id 的话，客户端只能再发一次 EnterMap。
        if (have_live_place) {
            out->set_map_instance_id(live_place.map_instance_id);
            if (live_place.map_template_id != 0)
                out->set_map_template_id(live_place.map_template_id);
            if (live_place.realm_id != 0)
                out->set_realm_id(live_place.realm_id);
            if (live_place.owner_epoch != 0)
                out->set_owner_epoch(live_place.owner_epoch);
            if (live_place.route_version != 0)
                out->set_route_version(live_place.route_version);
            if (!live_place.owner_logic_server_id.empty())
                out->set_gamelogic_instance_id(live_place.owner_logic_server_id);
        }
#endif
        // 这个逻辑上没有该玩家的权威视野。ok=true 加空 aoi_entities 会被客户端当成全量替换，把周围的人删掉。
        out->set_ok(false);
        out->clear_aoi_entities();
        out->set_error_code(gameproto::kErrAoiResyncRequired);
        out->set_message("snapshot not on this owner");
        LOG_INFO << "full snapshot not on owner player=" << player_id
                 << " map=" << out->map_instance_id()
                 << " logic=" << out->gamelogic_instance_id()
                 << " baseline=" << baseline_server_seq;
        return false;
    }
    out->set_ok(true);
    out->set_error_code("OK");
    out->set_message("ok");
    return true;
}

void GameLogic::SetLifeStateForTest(uint64_t player_id, int32_t hp, const std::string &life_state) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    auto &p = profiles_[player_id];
    p.set_player_id(player_id);
    p.set_hp(hp);
    if (p.max_hp() <= 0)
        p.set_max_hp(100);
    p.set_life_state(life_state);
}

void GameLogic::MaybePersistLastSafe(uint64_t player_id, const MapEntity &e,
                                     uint64_t map_instance_id, bool force) {
    const uint64_t tpl = MapInstanceRegistry::Instance().TemplateId(map_instance_id);
    auto static_data = MapCatalog::Instance().Get(tpl);
    if (!static_data || !static_data->InBounds(e.x, e.y, e.z) || !static_data->IsWalkable(e.x, e.z))
        return;
    int64_t flush_ms = 5000;
    float flush_dist = 4.f;
    if (const char *v = std::getenv("GAMEMESH_SAFE_POS_FLUSH_MS")) {
        const int n = std::atoi(v);
        if (n >= 0 && n <= 120000)
            flush_ms = n;
    }
    if (const char *v = std::getenv("GAMEMESH_SAFE_POS_FLUSH_DIST")) {
        const float d = std::strtof(v, nullptr);
        if (d > 0.f && d < 1000.f)
            flush_dist = d;
    }
    LastSafeMem mem;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto &slot = last_safe_[player_id];
        slot.map_instance_id = map_instance_id;
        if (tpl != 0)
            slot.map_template_id = tpl;
        const float dx = e.x - slot.x, dy = e.y - slot.y, dz = e.z - slot.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        slot.x = e.x;
        slot.y = e.y;
        slot.z = e.z;
        slot.yaw = e.yaw;
        slot.dirty = true;
        const int64_t now = NowMs();
        if (!force && slot.last_flush_ms != 0 && (now - slot.last_flush_ms) < flush_ms &&
            dist < flush_dist)
            return;
        if (!force)
            slot.last_flush_ms = now;
        mem = slot;
    }
    (void)mem;
#ifdef WEBSERVER_ENABLE_BRPC
    // 权威坐标已经写进 MapRuntime。节流落库走卸载池，避免同步 GameDB 占住这条线的 MoveRsp。
    if (!force) {
        if (RpcOffloadPool::Instance().TryPost([player_id] {
                GameLogic::Instance().FlushLastSafe(player_id, "throttle");
            })) {
            OpsMetrics::Instance().IncSafePosDeferred();
            return;
        }
        OpsMetrics::Instance().IncSafePosDeferFail();
        std::lock_guard<std::mutex> lk(mu_);
        last_safe_[player_id].last_flush_ms = 0;
        return;
    }
#endif
    FlushLastSafe(player_id, force ? "force" : "throttle");
}

void GameLogic::FlushLastSafe(uint64_t player_id, const char *reason) {
    LastSafeMem mem;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = last_safe_.find(player_id);
        if (it == last_safe_.end() || !it->second.dirty || it->second.map_template_id == 0)
            return;
        mem = it->second;
    }
#ifdef WEBSERVER_ENABLE_BRPC
    if (BrpcGameDbRepository::Instance().started()) {
        BrpcGameDbRepository::LastSafePos pos;
        pos.player_id = player_id;
        pos.realm_id = mem.realm_id;
        pos.map_template_id = mem.map_template_id;
        pos.x = mem.x;
        pos.y = mem.y;
        pos.z = mem.z;
        pos.yaw = mem.yaw;
        uint64_t ver = 0;
        bool skipped = false;
        std::string err;
        if (BrpcGameDbRepository::Instance().SaveLastSafePosition(pos, 0, &ver, &skipped, &err) &&
            !skipped) {
            std::lock_guard<std::mutex> lk(mu_);
            auto &slot = last_safe_[player_id];
            slot.position_version = ver;
            slot.last_flush_ms = NowMs();
            slot.dirty = false;
            return;
        }
        if (!err.empty())
            LOG_WARN << "FlushLastSafe failed player=" << player_id << " err=" << err;
    }
#else
    (void)reason;
#endif
    std::lock_guard<std::mutex> lk(mu_);
    last_safe_[player_id].last_flush_ms = NowMs();
}

void GameLogic::FlushAllLastSafe(const char *reason) {
    std::vector<uint64_t> ids = MapRuntime::Instance().ListPlayers();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &kv : last_safe_) {
            if (kv.second.dirty)
                ids.push_back(kv.first);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (uint64_t pid : ids) {
        MapEntity self;
        uint64_t mid = 0;
        if (MapRuntime::Instance().SnapshotView(pid, &self, nullptr, &mid))
            MaybePersistLastSafe(pid, self, mid, true);
        else
            FlushLastSafe(pid, reason);
    }
}

bool GameLogic::HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp) {
    LOG_INFO << "[consume_item] recv player_id=" << req.player_id() << " item_id=" << req.item_id()
             << " count=" << req.count();
    if (req.count() == 0) {
        rsp->set_ok(false);
        rsp->set_message("count must be > 0");
        return false;
    }
    if (!EnsureAssetsSynced(req.player_id())) {
        rsp->set_ok(false);
        rsp->set_message("STATE_SYNC_REQUIRED");
        auto *body = rsp->mutable_consume_item();
        body->set_ok(false);
        body->set_message("STATE_SYNC_REQUIRED");
        return false;
    }
#if defined(WEBSERVER_ENABLE_BRPC)
    if (FormalModeEnabled()) {
        if (!BrpcGameDbRepository::Instance().started()) {
            rsp->set_ok(false);
            rsp->set_message("gamedb unavailable");
            return false;
        }
        uint64_t expect = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            EnsurePlayer(req.player_id());
            expect = asset_version_[req.player_id()];
        }
        std::string idem = "logic:consume:";
        if (const ForwardRouteMeta *m = ForwardMetaContext::Get()) {
            idem += m->session_id + ":" + std::to_string(m->client_seq) + ":consume:" +
                    std::to_string(req.player_id()) + ":" + std::to_string(req.item_id()) + ":" +
                    std::to_string(req.count());
        } else {
            idem += std::to_string(req.player_id()) + ":" + std::to_string(req.item_id()) + ":" +
                    std::to_string(req.count()) + ":v" + std::to_string(expect);
        }
        BrpcGameDbRepository::AssetMutationResult mr;
        if (!BrpcGameDbRepository::Instance().ApplyAssetMutation(
                req.player_id(), idem, expect, "CONSUME", req.item_id(), req.count(), "", &mr)) {
            rsp->set_ok(false);
            rsp->set_message(mr.message.empty() ? mr.error_code : mr.message);
            auto *body = rsp->mutable_consume_item();
            body->set_ok(false);
            body->set_message(rsp->message());
            body->set_remain_count(mr.remain_count);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            EnsurePlayer(req.player_id());
            inventory_[req.player_id()][req.item_id()] = mr.remain_count;
            asset_version_[req.player_id()] = mr.asset_version;
        }
        rsp->set_ok(true);
        rsp->set_message("item consumed");
        auto *body = rsp->mutable_consume_item();
        body->set_ok(true);
        body->set_message("ok");
        body->set_remain_count(mr.remain_count);
#if defined(WEBSERVER_ENABLE_BRPC)
        game::GameResponse notify;
        notify.set_ok(true);
        notify.set_message("item_changed");
        *notify.mutable_consume_item() = *body;
        std::string payload;
        if (notify.SerializeToString(&payload))
            GameLogicPush::PushToBoundGateway("", req.player_id(), "", "item_changed", payload, true,
                                              true, 0);
#endif
        return true;
    }
#endif
    uint32_t remain = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        EnsurePlayer(req.player_id());
        auto &inv = inventory_[req.player_id()];
        if (inv[req.item_id()] < req.count()) {
            rsp->set_ok(false);
            std::ostringstream os;
            os << "not enough item " << req.item_id() << ", have " << inv[req.item_id()];
            rsp->set_message(os.str());
            auto *body = rsp->mutable_consume_item();
            body->set_ok(false);
            body->set_message(rsp->message());
            body->set_remain_count(inv[req.item_id()]);
            LOG_WARN << "[consume_item] fail player_id=" << req.player_id()
                     << " item_id=" << req.item_id() << " have=" << inv[req.item_id()]
                     << " need=" << req.count();
            return false;
        }
        inv[req.item_id()] -= req.count();
        ++asset_version_[req.player_id()];
        remain = inv[req.item_id()];
    }
    rsp->set_ok(true);
    rsp->set_message("item consumed");
    auto *body = rsp->mutable_consume_item();
    body->set_ok(true);
    body->set_message("ok");
    body->set_remain_count(remain);
    LOG_INFO << "[consume_item] ok player_id=" << req.player_id() << " item_id=" << req.item_id()
             << " remain=" << remain;
#if defined(WEBSERVER_ENABLE_BRPC)
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_message("item_changed");
    *notify.mutable_consume_item() = *body;
    std::string payload;
    if (notify.SerializeToString(&payload))
        GameLogicPush::PushToBoundGateway("", req.player_id(), "", "item_changed", payload, true,
                                          true, 0);
#endif
    return true;
}

bool GameLogic::HandleReleaseSkill(const game::ReleaseSkillReq &req, game::GameResponse *rsp) {
    LOG_INFO << "[release_skill] recv player_id=" << req.player_id() << " skill_id=" << req.skill_id()
             << " target=(" << req.target_x() << "," << req.target_y() << ")";
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(req.player_id());
    const int64_t now = NowMs();
    auto &cds = skill_cd_until_ms_[req.player_id()];
    if (cds[req.skill_id()] > now) {
        const uint32_t left = static_cast<uint32_t>(cds[req.skill_id()] - now);
        rsp->set_ok(false);
        rsp->set_message("skill on cooldown");
        auto *body = rsp->mutable_release_skill();
        body->set_ok(false);
        body->set_message("cooldown");
        body->set_cooldown_ms(left);
        LOG_WARN << "[release_skill] cooldown player_id=" << req.player_id()
                 << " skill_id=" << req.skill_id() << " left_ms=" << left;
        return false;
    }
    const int64_t cd = (req.skill_id() == 2001) ? 3000 : 1500;
    cds[req.skill_id()] = now + cd;
    std::ostringstream os;
    os << "skill " << req.skill_id() << " cast at (" << req.target_x() << "," << req.target_y()
       << ")";
    rsp->set_ok(true);
    rsp->set_message(os.str());
    auto *body = rsp->mutable_release_skill();
    body->set_ok(true);
    body->set_message("cast ok");
    body->set_cooldown_ms(static_cast<uint32_t>(cd));
    LOG_INFO << "[release_skill] ok player_id=" << req.player_id() << " skill_id=" << req.skill_id()
             << " cooldown_ms=" << cd << " " << os.str();
    return true;
}

// 道具发放业务：
//   1) 校验参数并更新内存 inventory_（与 consume_item 同一套聚合背包）
//   2) 构造 PendingPlayerItem 入队，由 PlayerItemPersistQueue 每 5 分钟或 logout 写入 player_item
//   3) 响应 instance_id=0 表示尚未落库；bag_total 为当前内存中该道具总数
bool GameLogic::HandleGrantItem(const game::GrantItemReq &req, game::GameResponse *rsp) {
    LOG_INFO << "[grant_item] recv player_id=" << req.player_id() << " item_id=" << req.item_id()
             << " count=" << req.count();
    auto *body = rsp->mutable_grant_item();
    // 公网命令面封闭：仅显式联调开关允许（Formal 恒拒绝）
    if (!AllowUnsafeDebugCommandsEnv()) {
        rsp->set_ok(false);
        rsp->set_message("ERR_COMMAND_FORBIDDEN");
        body->set_ok(false);
        body->set_message(rsp->message());
        return false;
    }
    if (!EnsureAssetsSynced(req.player_id())) {
        rsp->set_ok(false);
        rsp->set_message("STATE_SYNC_REQUIRED");
        body->set_ok(false);
        body->set_message("STATE_SYNC_REQUIRED");
        return false;
    }
    if (req.player_id() == 0 || req.item_id() == 0 || req.count() == 0) {
        rsp->set_ok(false);
        rsp->set_message("invalid player_id, item_id or count");
        body->set_ok(false);
        body->set_message(rsp->message());
        return false;
    }
    if (req.item_id() > 0xFFFFFFFFu) {
        rsp->set_ok(false);
        rsp->set_message("item_id too large");
        body->set_ok(false);
        body->set_message(rsp->message());
        return false;
    }
    const uint32_t item_id = static_cast<uint32_t>(req.item_id());
    uint32_t bag_total = 0;

#if defined(WEBSERVER_ENABLE_BRPC)
    if (FormalModeEnabled()) {
        if (!BrpcGameDbRepository::Instance().started()) {
            rsp->set_ok(false);
            rsp->set_message("gamedb unavailable");
            body->set_ok(false);
            body->set_message(rsp->message());
            return false;
        }
        uint64_t expect = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            EnsurePlayer(req.player_id());
            expect = asset_version_[req.player_id()];
        }
        std::string idem = "logic:grant:";
        if (const ForwardRouteMeta *m = ForwardMetaContext::Get()) {
            idem += m->session_id + ":" + std::to_string(m->client_seq) + ":grant:" +
                    std::to_string(req.player_id()) + ":" + std::to_string(item_id) + ":" +
                    std::to_string(req.count());
        } else {
            idem += std::to_string(req.player_id()) + ":" + std::to_string(item_id) + ":" +
                    std::to_string(req.count()) + ":v" + std::to_string(expect);
        }
        BrpcGameDbRepository::AssetMutationResult mr;
        if (!BrpcGameDbRepository::Instance().ApplyAssetMutation(
                req.player_id(), idem, expect, "GRANT", item_id, req.count(), "", &mr)) {
            rsp->set_ok(false);
            rsp->set_message(mr.message.empty() ? mr.error_code : mr.message);
            body->set_ok(false);
            body->set_message(rsp->message());
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            EnsurePlayer(req.player_id());
            inventory_[req.player_id()][item_id] = mr.remain_count;
            asset_version_[req.player_id()] = mr.asset_version;
            bag_total = mr.remain_count;
        }
        rsp->set_ok(true);
        rsp->set_message("item granted (gamedb)");
        body->set_ok(true);
        body->set_message(mr.idempotent_hit ? "idempotent" : "ok");
        body->set_instance_id(0);
        body->set_bag_total(bag_total);
#if defined(WEBSERVER_ENABLE_BRPC)
        game::GameResponse notify;
        notify.set_ok(true);
        notify.set_message("item_changed");
        *notify.mutable_grant_item() = *body;
        std::string payload;
        if (notify.SerializeToString(&payload))
            GameLogicPush::PushToBoundGateway("", req.player_id(), "", "item_changed", payload, true,
                                              true, 0);
#endif
        return true;
    }
#endif

#ifdef WEBSERVER_ENABLE_MYSQL
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        rsp->set_ok(false);
        rsp->set_message("mysql pool not initialized");
        body->set_ok(false);
        body->set_message(rsp->message());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        EnsurePlayer(req.player_id());
        inventory_[req.player_id()][item_id] += req.count();
        ++asset_version_[req.player_id()];
        bag_total = inventory_[req.player_id()][item_id];
    }
    PendingPlayerItem pending;
    pending.player_id = req.player_id();
    pending.item_id = req.item_id();
    pending.count = req.count();
    pending.expire_time_sec = req.expire_time_sec();
    pending.extra_data = req.extra_data();
    PlayerItemPersistQueue::Instance().Enqueue(pending);
    PlayerItemPersistQueue::Instance().MarkOnline(req.player_id());
#else
    (void)item_id;
    rsp->set_ok(false);
    rsp->set_message("mysql not enabled");
    body->set_ok(false);
    body->set_message(rsp->message());
    return false;
#endif

    rsp->set_ok(true);
    rsp->set_message("item granted (queued, persist every 5min)");
    body->set_ok(true);
    body->set_message("queued");
    body->set_instance_id(0);
    body->set_bag_total(bag_total);
    LOG_INFO << "[grant_item] ok player_id=" << req.player_id() << " item_id=" << item_id
             << " bag_total=" << bag_total << " (db queued)";
#if defined(WEBSERVER_ENABLE_BRPC)
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_message("item_changed");
    *notify.mutable_grant_item() = *body;
    std::string payload;
    if (notify.SerializeToString(&payload))
        GameLogicPush::PushToBoundGateway("", req.player_id(), "", "item_changed", payload, true,
                                          true, 0);
#endif
    return true;
}

bool GameLogic::RequireSessionToken(const game::GameRequest &req, uint64_t player_id,
                                    game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_REDIS
    std::string err;
#ifdef WEBSERVER_ENABLE_BRPC
    if (SessionRpcClient::Instance().ready()) {
        if (!SessionRpcClient::Instance().ValidateToken(player_id, req.session_token(), &err)) {
            const std::string why = err.empty() ? "session invalid" : err;
            rsp->set_ok(false);
            rsp->set_message(why);
            if (why.find("ONLINE") != std::string::npos || why.find("disconnected") != std::string::npos ||
                why.find("session invalid") != std::string::npos ||
                why.find("invalid session token") != std::string::npos)
                rsp->set_error_code(gameproto::kErrFenceStale);
            else if (why.find("expired") != std::string::npos || why.find("not logged") != std::string::npos ||
                     why.find("not found") != std::string::npos)
                rsp->set_error_code(gameproto::kErrSessionExpired);
            LOG_WARN << "session token rejected player_id=" << player_id << " msg=" << why;
            return false;
        }
        return true;
    }
#endif
    if (!SessionStore::Instance().Available())
        return true;
    if (!SessionStore::Instance().ValidateToken(player_id, req.session_token(), &err)) {
        const std::string why = err.empty() ? "session invalid" : err;
        rsp->set_ok(false);
        rsp->set_message(why);
        if (why.find("ONLINE") != std::string::npos || why.find("disconnected") != std::string::npos ||
            why.find("session invalid") != std::string::npos ||
            why.find("invalid session token") != std::string::npos)
            rsp->set_error_code(gameproto::kErrFenceStale);
        else if (why.find("expired") != std::string::npos || why.find("not logged") != std::string::npos ||
                 why.find("not found") != std::string::npos)
            rsp->set_error_code(gameproto::kErrSessionExpired);
        LOG_WARN << "session token rejected player_id=" << player_id << " msg=" << why;
        return false;
    }
#else
    (void)req;
    (void)player_id;
    (void)rsp;
#endif
    return true;
}

bool GameLogic::HandleLogin(const game::LoginReq &req, game::GameResponse *rsp) {
    (void)req;
    auto *body = rsp->mutable_login();
    body->set_ok(false);
    body->set_message("login must be orchestrated by Gateway via Auth+Session; GameLogic rejects credentials");
    rsp->set_ok(false);
    rsp->set_message(body->message());
    LOG_WARN << "HandleLogin rejected: GameLogic no longer performs Auth/Session Login RPC";
    return false;
}

namespace {

#ifdef WEBSERVER_ENABLE_MYSQL
void RowToAttributes(const PlayerProfileRow &row, game::PlayerAttributes *out) {
    if (!out)
        return;
    out->set_player_id(row.player_id);
    out->set_player_name(row.player_name);
    out->set_hp(row.hp);
    out->set_max_hp(row.max_hp);
    out->set_mp(row.mp);
    out->set_max_mp(row.max_mp);
    out->set_attack(row.attack);
    out->set_spell_power(row.spell_power);
    out->set_defense(row.defense);
    out->set_magic_resistance(row.magic_resistance);
    out->set_crit_chance(row.crit_chance);
    out->set_crit_damage(row.crit_damage);
    out->set_move_speed(row.move_speed);
    out->set_attack_speed(row.attack_speed);
    out->set_stats_version(row.stats_version);
}
#endif

void FillCompiledDefaultProfile(uint64_t player_id, game::PlayerAttributes *out) {
    if (!out)
        return;
    out->Clear();
    out->set_player_id(player_id);
    out->set_player_name("player");
    out->set_hp(100);
    out->set_max_hp(100);
    out->set_mp(100);
    out->set_max_mp(100);
    out->set_attack(10);
    out->set_spell_power(10);
    out->set_defense(5);
    out->set_magic_resistance(5);
    out->set_crit_chance(0.05f);
    out->set_crit_damage(1.5f);
    out->set_move_speed(10.0f);
    out->set_attack_speed(1.0f);
    out->set_stats_version(1);
}

}  // namespace

bool GameLogic::LoadProfileUnlocked(uint64_t player_id, game::PlayerAttributes *out,
                                    std::string *err) {
    if (!out) {
        if (err)
            *err = "bad arg";
        return false;
    }
#if defined(WEBSERVER_ENABLE_BRPC)
    if (FormalModeEnabled() && BrpcGameDbRepository::Instance().started()) {
        if (!BrpcGameDbRepository::Instance().LoadPlayerProfile(player_id, true, out, err)) {
            if (err && err->empty())
                *err = "ERR_PLAYER_LOAD_FAILED";
            return false;
        }
        return true;
    }
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
    if (PlayerProfileStore::Instance().Available() || PlayerProfileStore::Instance().EnsureTable()) {
        PlayerProfileRow row;
        std::string load_err;
        if (!PlayerProfileStore::Instance().Load(player_id, &row, &load_err)) {
            if (err)
                *err = load_err.empty() ? "profile load failed" : load_err;
            return false;
        }
        if (!row.exists) {
            AccountAuthRow acc;
            std::string name = "player";
            if (PlayerAccountStore::Instance().LoadAuth(player_id, &acc) && acc.exists &&
                !acc.display_name.empty())
                name = acc.display_name;
            if (!PlayerProfileStore::Instance().EnsureDefault(player_id, name, &load_err) ||
                !PlayerProfileStore::Instance().Load(player_id, &row, &load_err) || !row.exists) {
                if (err)
                    *err = load_err.empty() ? "ERR_PROFILE_NOT_FOUND" : load_err;
                return false;
            }
        }
        RowToAttributes(row, out);
        return true;
    }
#endif
    if (FormalModeEnabled()) {
        if (err)
            *err = "ERR_PLAYER_LOAD_FAILED";
        return false;
    }
    FillCompiledDefaultProfile(player_id, out);
    return true;
}

bool GameLogic::GetPlayerAttributes(uint64_t player_id, game::PlayerAttributes *out) {
    if (!out || player_id == 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = profiles_.find(player_id);
    if (it == profiles_.end())
        return false;
    *out = it->second;
    return true;
}

bool GameLogic::BindAuthenticatedPlayer(uint64_t player_id, std::string *err,
                                        game::PlayerAttributes *profile_out) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        player_load_ok_.erase(player_id);
        if (!EnsurePlayerLoaded(player_id, err))
            return false;
        auto it = profiles_.find(player_id);
        if (it != profiles_.end()) {
            if (profile_out)
                *profile_out = it->second;
#ifdef WEBSERVER_ENABLE_MYSQL
            if (!FormalModeEnabled())
                PlayerItemPersistQueue::Instance().MarkOnline(player_id);
#endif
            return true;
        }
    }
    game::PlayerAttributes prof;
    if (!LoadProfileUnlocked(player_id, &prof, err)) {
        std::lock_guard<std::mutex> lk(mu_);
        player_load_ok_[player_id] = false;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        profiles_[player_id] = prof;
#ifdef WEBSERVER_ENABLE_MYSQL
        if (!FormalModeEnabled())
            PlayerItemPersistQueue::Instance().MarkOnline(player_id);
#endif
    }
    if (profile_out)
        *profile_out = prof;
#ifdef WEBSERVER_ENABLE_BRPC
    if (BrpcGameDbRepository::Instance().started()) {
        BrpcGameDbRepository::LastSafePos pos;
        std::string lerr;
        if (BrpcGameDbRepository::Instance().LoadLastSafePosition(player_id, &pos, &lerr) &&
            pos.exists) {
            std::lock_guard<std::mutex> lk(mu_);
            auto &slot = last_safe_[player_id];
            slot.realm_id = pos.realm_id != 0 ? pos.realm_id : 1;
            slot.map_template_id = pos.map_template_id;
            slot.x = pos.x;
            slot.y = pos.y;
            slot.z = pos.z;
            slot.yaw = pos.yaw;
            slot.position_version = pos.position_version;
            slot.dirty = false;
        }
    }
#endif
    return true;
}

bool GameLogic::FlushBag(uint64_t player_id, const std::string &reason) {
    game::FlushBagReq req;
    req.set_player_id(player_id);
    req.set_reason(reason);
    game::GameResponse rsp;
    return HandleFlushBag(req, &rsp);
}

bool GameLogic::HandleReconnect(const game::ReconnectReq &req, game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_REDIS
    auto *body = rsp->mutable_reconnect();
#ifdef WEBSERVER_ENABLE_BRPC
    if (SessionRpcClient::Instance().ready()) {
        if (!SessionRpcClient::Instance().Reconnect(req, body)) {
            body->set_ok(false);
            body->set_message("session rpc failed");
        }
    } else
#endif
        SessionStore::Instance().Reconnect(req, body);
    rsp->set_ok(body->ok());
    rsp->set_message(body->message());
    if (body->ok()) {
#ifdef WEBSERVER_ENABLE_MYSQL
        PlayerItemPersistQueue::Instance().MarkOnline(req.player_id());
#endif
    }
    return body->ok();
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("redis not enabled");
    return false;
#endif
}

bool GameLogic::HandleValidateSession(const game::ValidateSessionReq &req, game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_REDIS
    auto *body = rsp->mutable_validate_session();
    SessionStore::Instance().Validate(req, body);
    rsp->set_ok(body->ok());
    rsp->set_message(body->message());
    return body->ok();
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("redis not enabled");
    return false;
#endif
}

bool GameLogic::HandleCheckOnline(const game::CheckOnlineReq &req, game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_REDIS
    auto *body = rsp->mutable_check_online();
    SessionStore::Instance().CheckOnline(req, body);
    rsp->set_ok(body->ok());
    rsp->set_message(body->message());
    return body->ok();
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("redis not enabled");
    return false;
#endif
}

bool GameLogic::HandleLogout(const game::LogoutReq &req, game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_REDIS
    auto *body = rsp->mutable_logout();
#ifdef WEBSERVER_ENABLE_BRPC
    if (SessionRpcClient::Instance().ready()) {
        if (!SessionRpcClient::Instance().Logout(req, body)) {
            body->set_ok(false);
            body->set_message("session rpc failed");
        }
    } else
#endif
        SessionStore::Instance().Logout(req, body);
    rsp->set_ok(body->ok());
    rsp->set_message(body->message());
    if (body->ok()) {
#ifdef WEBSERVER_ENABLE_MYSQL
        PlayerItemPersistQueue::Instance().MarkOffline(req.player_id());
#endif
        AoiPushBatch pushes;
        MapRuntime::Instance().LeaveAll(req.player_id(), &pushes);
        MapInstanceRegistry::Instance().RemovePlayerFromAll(req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
        PlacementStore::Instance().ReleaseByPlayer(req.player_id());
#endif
        EmitAoi(pushes);
        FlushLastSafe(req.player_id(), "logout");
        std::lock_guard<std::mutex> lk(mu_);
        inventory_.erase(req.player_id());
        skill_cd_until_ms_.erase(req.player_id());
        profiles_.erase(req.player_id());
    }
    return body->ok();
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("redis not enabled");
    return false;
#endif
}

bool GameLogic::HandleRegister(const game::RegisterReq &req, game::GameResponse *rsp) {
    (void)req;
    auto *body = rsp->mutable_register_();
    body->set_ok(false);
    body->set_message("register must be orchestrated by Gateway via Auth→GameDB; GameLogic rejects");
    rsp->set_ok(false);
    rsp->set_message(body->message());
    LOG_WARN << "HandleRegister rejected: use Gateway→Auth→GameDB";
    return false;
}

bool GameLogic::HandleFlushBag(const game::FlushBagReq &req, game::GameResponse *rsp) {
#ifdef WEBSERVER_ENABLE_MYSQL
    auto *body = rsp->mutable_flush_bag();
    PlayerItemPersistQueue::Instance().MarkOffline(req.player_id());
    body->set_ok(true);
    body->set_message(req.reason().empty() ? "flushed" : req.reason());
    rsp->set_ok(true);
    rsp->set_message(body->message());
    LOG_INFO << "FlushBag player_id=" << req.player_id() << " reason=" << req.reason();
    return true;
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("mysql not enabled");
    return false;
#endif
}

bool GameLogic::HandleEnterMap(const game::EnterMapReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_enter_map();
    body->set_ok(false);
    if (req.player_id() == 0 || req.map_template_id() == 0) {
        body->set_message("invalid player or template");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }

    MapPlacementRecord place;
    // brpc bthread 可能迁移 pthread：TLS ForwardMeta 在 Redis/RPC yield 后不可再解引用
    ForwardRouteMeta meta_snap;
    bool have_meta = false;
    if (const ForwardRouteMeta *meta = ForwardMetaContext::Get()) {
        meta_snap = *meta;
        have_meta = true;
    }
    const bool formalish =
        MapInstanceRegistry::Instance().require_lease() || FormalModeEnabled();

    if (have_meta && meta_snap.map_instance_id != 0) {
        if (formalish) {
#ifdef WEBSERVER_ENABLE_REDIS
            PlacementRecord auth;
            std::string ferr;
            if (!FetchAuthorityPlacement(meta_snap.map_instance_id, &auth, &ferr)) {
                body->set_message(ferr);
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
            std::string code;
            if (!ValidateLocalAuthorityWrite(auth, meta_snap.owner_epoch, meta_snap.route_version,
                                             &code, formalish)) {
                body->set_message(code);
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
            place.map_instance_id = auth.map_instance_id;
            place.owner_epoch = auth.owner_epoch;
            place.route_version = auth.route_version;
            place.owner_gamelogic_id = auth.owner_logic_server_id;
            place.map_template_id =
                auth.map_template_id != 0 ? auth.map_template_id : req.map_template_id();
            place.realm_id = auth.realm_id != 0 ? auth.realm_id : req.realm_id();
            place.lease_until_unix = auth.lease_until;
            place.kind = auth.kind;
            place.line_no = auth.line_no;
            place.soft_cap = auth.soft_cap;
            place.hard_cap = auth.hard_cap;
#else
            body->set_message("ERR_PLACEMENT_UNAVAILABLE");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
#endif
        } else {
            if (!meta_snap.gamelogic_instance_id.empty() &&
                meta_snap.gamelogic_instance_id !=
                    MapInstanceRegistry::Instance().local_instance_id()) {
                body->set_message("ERR_WRONG_GAMELOGIC_OWNER");
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
            place.map_instance_id = meta_snap.map_instance_id;
            place.owner_epoch = meta_snap.owner_epoch;
            place.route_version = meta_snap.route_version;
            place.owner_gamelogic_id = meta_snap.gamelogic_instance_id;
            place.map_template_id = req.map_template_id();
            place.realm_id = req.realm_id();
#ifdef WEBSERVER_ENABLE_REDIS
            PlacementRecord prec;
            std::string ignore;
            if (FetchAuthorityPlacement(place.map_instance_id, &prec, &ignore)) {
                place.lease_until_unix = prec.lease_until;
                place.kind = prec.kind;
                place.line_no = prec.line_no;
                place.soft_cap = prec.soft_cap;
                place.hard_cap = prec.hard_cap;
            }
#endif
        }
    } else if (formalish) {
        // Formal：GameLogic 不得 ResolveOrCreate 隐式 Claim；必须已有权威实例
#ifdef WEBSERVER_ENABLE_REDIS
        if (req.map_instance_id() == 0) {
            body->set_message("ERR_PLACEMENT_REQUIRED");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        PlacementRecord auth;
        std::string ferr;
        if (!FetchAuthorityPlacement(req.map_instance_id(), &auth, &ferr)) {
            body->set_message(ferr);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        std::string code;
        if (!ValidateLocalAuthorityWrite(auth, 0, 0, &code, formalish)) {
            body->set_message(code);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        place.map_instance_id = auth.map_instance_id;
        place.owner_epoch = auth.owner_epoch;
        place.route_version = auth.route_version;
        place.owner_gamelogic_id = auth.owner_logic_server_id;
        place.map_template_id =
            auth.map_template_id != 0 ? auth.map_template_id : req.map_template_id();
        place.realm_id = auth.realm_id != 0 ? auth.realm_id : req.realm_id();
        place.lease_until_unix = auth.lease_until;
        place.kind = auth.kind;
        place.line_no = auth.line_no;
        place.soft_cap = auth.soft_cap;
        place.hard_cap = auth.hard_cap;
#else
        body->set_message("ERR_PLACEMENT_UNAVAILABLE");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
#endif
    } else {
#ifdef WEBSERVER_ENABLE_REDIS
        if (PlacementStore::Instance().Available()) {
            ResolveOrCreateInput in;
            in.realm_id = req.realm_id();
            in.map_template_id = req.map_template_id();
            in.map_instance_id = req.map_instance_id();
            ResolveOrCreateResult pout;
            if (!PlacementStore::Instance().ResolveOrCreate(in, &pout) || !pout.ok) {
                body->set_message(pout.message.empty() ? "placement failed" : pout.message);
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
            place.map_instance_id = pout.placement.map_instance_id;
            place.owner_epoch = pout.placement.owner_epoch;
            place.route_version = pout.placement.route_version;
            place.owner_gamelogic_id = pout.placement.owner_logic_server_id;
            place.map_template_id = pout.placement.map_template_id;
            place.realm_id = pout.placement.realm_id;
            place.lease_until_unix = pout.placement.lease_until;
            place.kind = pout.placement.kind;
            place.line_no = pout.placement.line_no;
            place.soft_cap = pout.placement.soft_cap;
            place.hard_cap = pout.placement.hard_cap;
        } else
#endif
        {
            if (MapPlacement::Instance().owners().empty())
                MapPlacement::Instance().ConfigureOwners(
                    {MapInstanceRegistry::Instance().local_instance_id()});
            if (!MapPlacement::Instance().ResolveOrAllocate(req.realm_id(), req.map_template_id(),
                                                           req.map_instance_id(), &place)) {
                body->set_message("placement failed");
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
        }
        if (place.owner_gamelogic_id != MapInstanceRegistry::Instance().local_instance_id()) {
            body->set_message("ERR_WRONG_GAMELOGIC_OWNER");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
    }

    if (formalish && place.lease_until_unix <= 0) {
        body->set_message("ERR_LEASE_MISSING");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }

    if (!MapInstanceRegistry::Instance().Claim(place.map_instance_id, place.map_template_id,
                                               place.owner_epoch, place.lease_until_unix)) {
        body->set_message("claim rejected");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    {
        const uint64_t old_mid = MapRuntime::Instance().PlayerMap(req.player_id());
        if (old_mid != 0 && old_mid != place.map_instance_id)
            MapInstanceRegistry::Instance().RemovePlayer(old_mid, req.player_id());
    }
    MapInstanceRegistry::Instance().AddPlayer(place.map_instance_id, req.player_id());

#ifdef WEBSERVER_ENABLE_REDIS
    // Redis occ 是分线权威。GameLogic 进程内 entity 在宽限过期后可能仍在，
    // 进图快照会把已下线 bot 同步给客户端（监控 1 人、画面几十个模型）。
    if (PlacementStore::Instance().Available()) {
        std::vector<uint64_t> occ;
        if (PlacementStore::Instance().ListOccupants(place.map_instance_id, &occ)) {
            std::unordered_set<uint64_t> keep(occ.begin(), occ.end());
            keep.insert(req.player_id());
            AoiPushBatch drops;
            const size_t n =
                MapRuntime::Instance().DropExcept(place.map_instance_id, keep, &drops);
            if (n > 0) {
                EmitAoi(drops);
                LOG_INFO << "EnterMap drop ghost entities map=" << place.map_instance_id
                         << " n=" << n << " occ=" << occ.size();
            }
        }
    }
#endif

    auto rollback_enter = [&]() {
        MapInstanceRegistry::Instance().RemovePlayer(place.map_instance_id, req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
        PlacementStore::Instance().ReleaseByPlayer(req.player_id());
#endif
    };

    std::shared_ptr<const MapStaticData> static_data =
        MapCatalog::Instance().Get(place.map_template_id);
    if (!static_data) {
        rollback_enter();
        body->set_message("ERR_MAP_UNKNOWN_TEMPLATE");
        body->set_map_template_id(place.map_template_id);
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    {
        const std::string client_hash = MapStaticData::NormalizeSha256Hex(req.map_data_sha256());
        const bool ver_bad = req.map_data_version() != 0 &&
                             req.map_data_version() != static_data->data_version();
        const bool hash_bad = !client_hash.empty() && client_hash != static_data->sha256();
        if (ver_bad || hash_bad) {
            rollback_enter();
            body->set_message("ERR_MAP_DATA_MISMATCH");
            body->set_map_template_id(static_data->map_template_id());
            body->set_map_data_version(static_data->data_version());
            body->set_map_data_sha256(static_data->sha256());
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
    }

    // Session 路由：Gateway 编排路径（meta 已带 map）由 Gateway Update/Transfer 写权威；
    // Logic 仅在无 meta 直连路径上自行 Update，避免 brpc yield 后二次 fence 校验踩踏。
    uint64_t route_ver = place.route_version;
#ifdef WEBSERVER_ENABLE_REDIS
    const bool gateway_routed = have_meta && meta_snap.map_instance_id != 0;
    const std::string fence = have_meta ? meta_snap.fence_token : std::string();
    if (!gateway_routed && !fence.empty()) {
        uint64_t rv = 0;
        std::string err;
        bool route_ok = false;
        const uint64_t pid = (have_meta && meta_snap.player_id != 0) ? meta_snap.player_id
                                                                     : req.player_id();
#ifdef WEBSERVER_ENABLE_BRPC
        if (SessionRpcClient::Instance().ready()) {
            sess::UpdatePlayerRouteRequest ureq;
            ureq.set_player_id(pid);
            ureq.set_fence_token(fence);
            ureq.set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());
            ureq.set_map_instance_id(place.map_instance_id);
            ureq.set_map_owner_epoch(place.owner_epoch);
            ureq.set_route_version(0);
            sess::UpdatePlayerRouteResponse ursp;
            route_ok = SessionRpcClient::Instance().UpdatePlayerRoute(ureq, &ursp) && ursp.ok();
            if (route_ok)
                rv = ursp.route_version();
            else
                err = ursp.message();
        } else
#endif
            if (SessionStore::Instance().Available()) {
            route_ok = SessionStore::Instance().UpdatePlayerRoute(
                pid, fence, MapInstanceRegistry::Instance().local_instance_id(),
                place.map_instance_id, place.owner_epoch, 0, "", "", &rv, &err);
        } else {
            route_ok = true;
        }
        if (!route_ok) {
            rollback_enter();
            body->set_message(err.empty() ? "update route failed" : err);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        if (rv != 0)
            route_ver = rv;
    } else if (gateway_routed && meta_snap.route_version != 0) {
        route_ver = meta_snap.route_version;
    }
#endif

    body->set_ok(true);
    body->set_message("entered");
    body->set_map_template_id(place.map_template_id);
    body->set_map_instance_id(place.map_instance_id);
    body->set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());
    body->set_owner_epoch(place.owner_epoch);
    body->set_route_version(route_ver);
    body->set_kind(place.kind.empty() ? "LEGACY_POOL" : place.kind);
    body->set_line_no(place.line_no);
    body->set_soft_cap(place.soft_cap);
    body->set_hard_cap(place.hard_cap);
#ifdef WEBSERVER_ENABLE_REDIS
    body->set_occupancy(PlacementStore::Instance().Occupancy(place.map_instance_id));
    if (body->kind() == "LINE")
        MapLineView::FillLines(req.realm_id(), place.map_template_id, body->mutable_lines());
#endif

    if (static_data) {
        body->set_map_data_version(static_data->data_version());
        body->set_map_data_sha256(static_data->sha256());

        game::PlayerAttributes attrs;
        GetPlayerAttributes(req.player_id(), &attrs);
        const MapSpawnPoint &sp = static_data->default_spawn();
        float sx = sp.position.x, sy = sp.position.y, sz = sp.position.z, syaw = sp.yaw;
        std::string recovery = "SPAWN_FALLBACK";
        if (MapRuntime::Instance().HasPlayer(place.map_instance_id, req.player_id()) ||
            MapRuntime::Instance().PlayerMap(req.player_id()) == place.map_instance_id) {
            MapEntity self_now;
            std::vector<MapEntity> aoi_now;
            uint64_t mid_now = 0;
            if (MapRuntime::Instance().SnapshotView(req.player_id(), &self_now, &aoi_now, &mid_now)) {
                if (!self_now.connected) {
                    std::string gw, sid;
#ifdef WEBSERVER_ENABLE_BRPC
                    GameLogicGetBoundMeta(req.player_id(), &gw, &sid, nullptr, nullptr);
#endif
                    if (have_meta && !meta_snap.session_id.empty())
                        sid = meta_snap.session_id;
                    AoiPushBatch pushes;
                    if (MapRuntime::Instance().Reconnect(req.player_id(), gw, sid, &self_now,
                                                         &aoi_now, &pushes))
                        EmitAoi(pushes);
                }
                body->set_ok(true);
                body->set_message("entered");
                body->set_map_template_id(place.map_template_id);
                body->set_map_instance_id(place.map_instance_id);
                body->set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());
                body->set_owner_epoch(place.owner_epoch);
                body->set_route_version(route_ver);
                body->set_kind(place.kind.empty() ? "LEGACY_POOL" : place.kind);
                body->set_line_no(place.line_no);
                body->set_soft_cap(place.soft_cap);
                body->set_hard_cap(place.hard_cap);
#ifdef WEBSERVER_ENABLE_REDIS
                body->set_occupancy(PlacementStore::Instance().Occupancy(place.map_instance_id));
                if (body->kind() == "LINE")
                    MapLineView::FillLines(req.realm_id(), place.map_template_id,
                                          body->mutable_lines());
#endif
                body->mutable_spawn_position()->set_x(self_now.x);
                body->mutable_spawn_position()->set_y(self_now.y);
                body->mutable_spawn_position()->set_z(self_now.z);
                body->set_spawn_yaw(self_now.yaw);
                body->set_map_data_version(static_data->data_version());
                body->set_map_data_sha256(static_data->sha256());
                FillEntitySnapshot(self_now, body->mutable_self());
                for (const auto &e : aoi_now)
                    FillEntitySnapshot(e, body->add_aoi_snapshot());
#ifdef WEBSERVER_ENABLE_REDIS
                PlacementStore::Instance().ConfirmSlot(req.player_id(), place.map_instance_id);
#endif
                rsp->set_ok(true);
                rsp->set_message("entered");
                return true;
            }
        }
        LastSafeMem cached;
        bool have_cached = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = last_safe_.find(req.player_id());
            if (it != last_safe_.end() && it->second.map_template_id == place.map_template_id) {
                cached = it->second;
                have_cached = true;
            }
        }
#ifdef WEBSERVER_ENABLE_BRPC
        if (!have_cached && BrpcGameDbRepository::Instance().started()) {
            BrpcGameDbRepository::LastSafePos pos;
            std::string lerr;
            if (BrpcGameDbRepository::Instance().LoadLastSafePosition(req.player_id(), &pos, &lerr) &&
                pos.exists && pos.map_template_id == place.map_template_id) {
                cached.realm_id = pos.realm_id;
                cached.map_template_id = pos.map_template_id;
                cached.x = pos.x;
                cached.y = pos.y;
                cached.z = pos.z;
                cached.yaw = pos.yaw;
                cached.position_version = pos.position_version;
                have_cached = true;
            }
        }
#endif
        if (have_cached && static_data->InBounds(cached.x, cached.y, cached.z) &&
            static_data->IsWalkable(cached.x, cached.z)) {
            sx = cached.x;
            sy = cached.y;
            sz = cached.z;
            syaw = cached.yaw;
            // 上次安全点若贴在 Y 盒底，那是坠落被记下来的，进图时抬回出生高度。
            if (sy <= static_data->bounds_min().y + 2.f)
                sy = static_data->default_spawn().position.y;
            recovery = "REENTER_FROM_SAFE_POS";
        } else if (have_cached) {
            recovery = "SPAWN_FALLBACK";
        }
        MapScenePolicy pol;
        const bool have_pol = MapCatalog::Instance().GetScenePolicy(place.map_template_id, &pol);
        // 1002 先落到模板出生点，再按 spawn_scatter_radius 打散，避免全员叠同一坐标。
        // 已经有可走的安全点时不再拉回模板出生点。全量快照会整表替换，旧出生点会把人拽回去。
        if (place.map_template_id == 1002 && recovery != "REENTER_FROM_SAFE_POS") {
            ApplyTemplateSpawn(place.map_template_id, &sx, &sy, &sz);
            recovery = "SPAWN_FALLBACK";
        }
        if (recovery == "SPAWN_FALLBACK" && have_pol && pol.spawn_scatter_radius > 0.f) {
            float nx = sx, ny = sy, nz = sz;
            if (static_data->FindScatteredSpawn(sx, sy, sz, pol.spawn_scatter_radius,
                                                req.player_id(), &nx, &ny, &nz)) {
                sx = nx;
                sy = ny;
                sz = nz;
            }
        }
        const int view_r = (have_pol && pol.aoi_view_radius_cells >= 0) ? pol.aoi_view_radius_cells
                                                                       : -1;
        const int max_vis = have_pol ? pol.aoi_max_visible : -1;
        MapEntity me;
        me.player_id = req.player_id();
        me.player_name = attrs.player_name();
        me.x = sx;
        me.y = sy;
        me.z = sz;
        me.yaw = syaw;
        me.hp = attrs.hp();
        me.max_hp = attrs.max_hp();
        me.move_speed = attrs.move_speed() > 0.f ? attrs.move_speed() : 10.f;
#ifdef WEBSERVER_ENABLE_BRPC
        GameLogicGetBoundMeta(req.player_id(), &me.gateway_instance_id, &me.session_id, nullptr,
                              nullptr);
#endif
        if (have_meta && !meta_snap.session_id.empty())
            me.session_id = meta_snap.session_id;
        MapEntity self;
        std::vector<MapEntity> snap;
        AoiPushBatch pushes;
        std::string rerr;
        if (!MapRuntime::Instance().Enter(place.map_instance_id, static_data, me, &self, &snap,
                                          &pushes, &rerr, view_r, max_vis)) {
            rollback_enter();
            body->set_ok(false);
            body->set_message(rerr.empty() ? "map runtime enter failed" : rerr);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        body->mutable_spawn_position()->set_x(self.x);
        body->mutable_spawn_position()->set_y(self.y);
        body->mutable_spawn_position()->set_z(self.z);
        body->set_spawn_yaw(self.yaw);
        FillEntitySnapshot(self, body->mutable_self());
        for (const auto &e : snap)
            FillEntitySnapshot(e, body->add_aoi_snapshot());
        EmitAoi(pushes);
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto &slot = last_safe_[req.player_id()];
            slot.realm_id = place.realm_id != 0 ? static_cast<uint32_t>(place.realm_id) : 1;
            slot.map_template_id = place.map_template_id;
            slot.map_instance_id = place.map_instance_id;
            slot.x = self.x;
            slot.y = self.y;
            slot.z = self.z;
            slot.yaw = self.yaw;
            slot.dirty = true;
            // 清掉节流时间，让下面这次异步落库一定会发出。不能在地图线程里同步写库。
            slot.last_flush_ms = 0;
        }
        MaybePersistLastSafe(req.player_id(), self, place.map_instance_id, false);
        static std::atomic<uint32_t> enter_recovery_logs{0};
        if ((enter_recovery_logs.fetch_add(1, std::memory_order_relaxed) & 31) == 0) {
            LOG_INFO << "[enter_map] recovery=" << recovery << " player=" << req.player_id();
        }
    }
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementStore::Instance().ConfirmSlot(req.player_id(), place.map_instance_id);
#endif
    rsp->set_ok(true);
    rsp->set_message("entered");
    static std::atomic<uint32_t> enter_ok_logs{0};
    if ((enter_ok_logs.fetch_add(1, std::memory_order_relaxed) & 31) == 0) {
        LOG_INFO << "[enter_map] player=" << req.player_id() << " map=" << place.map_instance_id
                 << " epoch=" << place.owner_epoch
                 << " gl=" << MapInstanceRegistry::Instance().local_instance_id();
    }
    // 进图结果只走这次 Dispatch 的 EnterMapRsp（客户端按请求 seq 匹配）。
    // 先 Push enter_map_notify 会在同一条 Gateway 连接上排到回包前面；那一帧若只写进
    // 用户态 send_buf，后面的 seq 回包和心跳都出不去，关闭时未读请求还会让 close() 发 RST。
    return true;
}

bool GameLogic::HandleInteractPortal(const game::InteractPortalReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_interact_portal();
    body->set_ok(false);
    body->set_portal_id(req.portal_id());
    MapEntity self;
    uint64_t mid = 0;
    if (!MapRuntime::Instance().SnapshotView(req.player_id(), &self, nullptr, &mid) || mid == 0) {
        body->set_message("not on map");
        body->set_error_code(gameproto::kErrNotOnMap);
        rsp->set_ok(false);
        rsp->set_error_code(gameproto::kErrNotOnMap);
        rsp->set_message(body->message());
        return false;
    }
    const uint64_t from = MapInstanceRegistry::Instance().TemplateId(mid);
    MapPortal portal;
    if (from == 0 || !MapCatalog::Instance().GetPortal(from, req.portal_id(), &portal)) {
        body->set_message("unknown portal");
        body->set_error_code(gameproto::kErrPortalUnknown);
        rsp->set_ok(false);
        rsp->set_error_code(gameproto::kErrPortalUnknown);
        rsp->set_message(body->message());
        return false;
    }
    if (!PlayerNearPortal(portal, self.x, self.z)) {
        body->set_message("too far from portal");
        body->set_error_code(gameproto::kErrPortalTooFar);
        rsp->set_ok(false);
        rsp->set_error_code(gameproto::kErrPortalTooFar);
        rsp->set_message(body->message());
        return false;
    }
    body->set_ok(true);
    body->set_message("ok");
    body->set_portal_id(portal.portal_id);
    body->set_from_map_template_id(from);
    body->set_map_template_id(portal.to_map_template_id);
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool GameLogic::HandleLeaveMap(const game::LeaveMapReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_leave_map();
    body->set_ok(false);
    if (req.map_instance_id() == 0 ||
        !MapInstanceRegistry::Instance().PlayerOnMap(req.map_instance_id(), req.player_id())) {
        body->set_message("not on map");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    AoiPushBatch pushes;
    MapEntity self;
    uint64_t mid = req.map_instance_id();
    MapRuntime::Instance().SnapshotView(req.player_id(), &self, nullptr, &mid);
    MapRuntime::Instance().Leave(req.map_instance_id(), req.player_id(), &pushes);
    MapInstanceRegistry::Instance().RemovePlayer(req.map_instance_id(), req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementStore::Instance().ReleaseByPlayer(req.player_id());
#endif
    EmitAoi(pushes);
    if (self.player_id != 0)
        MaybePersistLastSafe(req.player_id(), self, mid != 0 ? mid : req.map_instance_id(), true);
    else
        FlushLastSafe(req.player_id(), "leave_map");
    body->set_ok(true);
    body->set_message("left");
    rsp->set_ok(true);
    rsp->set_message("left");
    return true;
}

bool GameLogic::HandleMapPing(const game::MapPingReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_map_ping();
    body->set_ok(false);
    ForwardRouteMeta meta_snap;
    bool have_meta = false;
    if (const ForwardRouteMeta *meta = ForwardMetaContext::Get()) {
        meta_snap = *meta;
        have_meta = true;
    }
    const bool formalish =
        MapInstanceRegistry::Instance().require_lease() || FormalModeEnabled();
    if (have_meta && meta_snap.map_instance_id != 0) {
        if (!MapInstanceRegistry::Instance().AcceptWrite(meta_snap.map_instance_id,
                                                        meta_snap.owner_epoch)) {
            body->set_message("ERR_STALE_EPOCH");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
#ifdef WEBSERVER_ENABLE_REDIS
        if (formalish) {
            PlacementRecord auth;
            std::string ferr;
            if (!FetchAuthorityPlacement(meta_snap.map_instance_id, &auth, &ferr)) {
                body->set_message(ferr);
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
            std::string code;
            if (!ValidateLocalAuthorityWrite(auth, meta_snap.owner_epoch, meta_snap.route_version,
                                             &code, formalish)) {
                body->set_message(code);
                rsp->set_ok(false);
                rsp->set_message(body->message());
                return false;
            }
        }
#else
        (void)formalish;
#endif
    }
    if (!MapInstanceRegistry::Instance().PlayerOnMap(req.map_instance_id(), req.player_id())) {
        body->set_message("not on map");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    body->set_ok(true);
    body->set_message("pong");
    body->set_owner_epoch(MapInstanceRegistry::Instance().Epoch(req.map_instance_id()));
    body->set_player_count(MapInstanceRegistry::Instance().PlayerCount(req.map_instance_id()));
    body->set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());
    rsp->set_ok(true);
    rsp->set_message("pong");
    return true;
}

bool GameLogic::HandleChatSend(const game::ChatSendReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_chat_send();
    auto fail = [&](const char *code, const char *msg) {
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_ok(false);
        rsp->set_error_code(code);
        rsp->set_message(msg);
        return false;
    };
    if (req.player_id() == 0)
        return fail("ERR_INVALID_ARGUMENT", "player_id required");
    if (!social::ValidWorldChannel(req.channel()))
        return fail("ERR_CHANNEL_FORBIDDEN", "channel not allowed");
    const bool whisper = social::IsWhisperChannel(req.channel());
    if (whisper && req.target_player_id() == 0)
        return fail("ERR_INVALID_ARGUMENT", "whisper requires target_player_id");
    if (whisper && req.target_player_id() == req.player_id())
        return fail("ERR_INVALID_ARGUMENT", "cannot whisper self");
    bool hide_whisper = false;
    if (whisper) {
        std::string gate_err;
        const FriendWhisperGate gate = FriendService::Instance().GateWhisper(
            req.player_id(), req.target_player_id(), &gate_err);
        if (gate == FriendWhisperGate::Reject) {
            const std::string code = gate_err.empty() ? std::string("ERR_ALREADY_BLOCKED") : gate_err;
            const bool unavailable = code == "ERR_DEPENDENCY_UNAVAILABLE";
            fail(code.c_str(), unavailable ? "whisper check unavailable" : "blocked");
            if (unavailable)
                rsp->set_retryable(true);
            return false;
        }
        hide_whisper = gate == FriendWhisperGate::Hide;
    }
#ifndef WEBSERVER_ENABLE_BRPC
    (void)hide_whisper;
#endif
    const size_t max_cp = static_cast<size_t>(EnvBoundedInt("GAMEMESH_CHAT_MAX_CP", 200, 8, 2000));
    const size_t max_bytes =
        static_cast<size_t>(EnvBoundedInt("GAMEMESH_CHAT_MAX_BYTES", 800, 16, 4096));
    std::string tec;
    if (!social::ValidWorldChatText(req.text(), max_cp, max_bytes, &tec))
        return fail(tec.empty() ? "ERR_TEXT_LENGTH" : tec.c_str(), "invalid chat text");
#ifndef WEBSERVER_ENABLE_REDIS
    return fail("ERR_DEPENDENCY_UNAVAILABLE", "chat requires redis");
#else
    if (!SessionStore::Instance().Available())
        return fail("ERR_DEPENDENCY_UNAVAILABLE", "chat requires redis");
    const int limit = EnvBoundedInt("GAMEMESH_CHAT_PER_PLAYER", 5, 1, 60);
    const int win = EnvBoundedInt("GAMEMESH_CHAT_WINDOW_SEC", 2, 1, 60);
    if (!SessionStore::Instance().ConsumeChatQuota(req.player_id(), limit, win))
        return fail("ERR_RATE_LIMITED", "chat rate limited");
    const uint64_t mid = SessionStore::Instance().NextWorldChatMessageId();
    if (mid == 0)
        return fail("ERR_DEPENDENCY_UNAVAILABLE", "chat seq unavailable");
    std::string sender_name;
    game::PlayerAttributes attrs;
    if (GetPlayerAttributes(req.player_id(), &attrs) && !attrs.player_name().empty())
        sender_name = attrs.player_name();
    if (sender_name.empty()) {
        std::string err;
        if (LoadProfileUnlocked(req.player_id(), &attrs, &err) && !attrs.player_name().empty())
            sender_name = attrs.player_name();
    }
    if (sender_name.empty())
        sender_name = "player";
    const std::string channel =
        req.channel().empty() ? (whisper ? "whisper" : "world") : req.channel();
    const int64_t now = NowMs();
#ifdef WEBSERVER_ENABLE_BRPC
    game::GameResponse inner;
    inner.set_ok(true);
    inner.set_seq(0);
    auto *n = inner.mutable_chat_notify();
    n->set_message_id(mid);
    n->set_sender_player_id(req.player_id());
    n->set_sender_name(sender_name);
    n->set_channel(channel);
    n->set_text(req.text());
    n->set_server_time_ms(now);
    if (whisper)
        n->set_target_player_id(req.target_player_id());
    std::string payload;
    if (inner.SerializeToString(&payload)) {
        std::vector<SessionStore::OnlinePushTarget> targets;
        if (whisper) {
            if (!hide_whisper) {
                SessionStore::OnlinePushTarget one;
                if (SessionStore::Instance().GetOnlinePushTarget(req.target_player_id(), &one))
                    targets.push_back(std::move(one));
            }
        } else {
            SessionStore::Instance().ListOnlinePushTargets(&targets, 256);
        }
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
            m->set_message_type(whisper ? "chat.whisper.v1" : "chat.world.v1");
            m->set_payload(payload);
            m->set_reliable(false);
            m->set_coalescable(false);
            m->set_fence_token(t.fence_token);
            m->set_generation(t.generation);
        }
        if (batches.empty() && !hide_whisper)
            LOG_WARN << "[chat] no online push targets sender=" << req.player_id();
        for (auto &kv : batches) {
            if (kv.second.messages_size() == 0)
                continue;
            gwpush::PushBatchResponse prsp;
            if (!GatewayPushClient::Instance().PushBatch(kv.first, kv.second, &prsp) ||
                !prsp.ok()) {
                LOG_WARN << "[chat] PushBatch failed gw=" << kv.first
                         << " n=" << kv.second.messages_size()
                         << " msg=" << (prsp.message().empty() ? "rpc" : prsp.message());
            }
        }
    }
#endif
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    body->set_message_id(mid);
    body->set_server_time_ms(now);
    body->set_channel(channel);
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
#endif
}

bool GameLogic::HandleFriendList(const game::FriendListReq &req, game::GameResponse *rsp) {
    return FriendService::Instance().HandleList(req, rsp);
}

static bool FillPublicBrief(uint64_t player_id, const std::string &exact_name, game::PlayerBrief *out,
                     std::string *err_code, std::string *err) {
    if (!out)
        return false;
#ifdef WEBSERVER_ENABLE_BRPC
    if (BrpcGameDbRepository::Instance().started()) {
        return BrpcGameDbRepository::Instance().LoadPlayerBrief(player_id, exact_name, out, err_code,
                                                                err);
    }
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
    PlayerProfileRow row;
    std::string code, msg;
    if (player_id != 0) {
        if (!PlayerProfileStore::Instance().Load(player_id, &row, &msg)) {
            if (err_code)
                *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
            if (err)
                *err = msg;
            return false;
        }
        if (!row.exists) {
            if (err_code)
                *err_code = "ERR_PROFILE_NOT_FOUND";
            if (err)
                *err = "not found";
            return false;
        }
    } else {
        if (!PlayerProfileStore::Instance().LoadByExactName(exact_name, &row, &msg, &code)) {
            if (err_code)
                *err_code = code.empty() ? "ERR_DEPENDENCY_UNAVAILABLE" : code;
            if (err)
                *err = msg;
            return false;
        }
        if (!row.exists) {
            if (err_code)
                *err_code = "ERR_NOT_FOUND";
            if (err)
                *err = "not found";
            return false;
        }
    }
    out->set_player_id(row.player_id);
    out->set_player_name(row.player_name);
    out->set_max_hp(row.max_hp);
    out->set_max_mp(row.max_mp);
    return true;
#else
    (void)player_id;
    (void)exact_name;
    if (err_code)
        *err_code = "ERR_DEPENDENCY_UNAVAILABLE";
    if (err)
        *err = "profile store unavailable";
    return false;
#endif
}

bool GameLogic::HandleGetPlayerBrief(const game::GetPlayerBriefReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_get_player_brief();
    auto fail = [&](const char *code, const char *msg) {
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_ok(false);
        rsp->set_error_code(code);
        rsp->set_message(msg);
        return false;
    };
    const uint64_t querier = req.player_id();
    if (querier == 0)
        return fail("ERR_UNAUTHENTICATED", "querier required");
    uint64_t target_id = req.target_player_id();
    std::string name;
    if (target_id == 0 && !req.player_name().empty()) {
        std::string nec;
        if (!social::NormalizePlayerName(req.player_name(), &name, &nec))
            return fail(nec.empty() ? "ERR_TEXT_LENGTH" : nec.c_str(), "invalid player_name");
#ifdef WEBSERVER_ENABLE_REDIS
        const int limit = EnvBoundedInt("GAMEMESH_NAME_QUERY_PER_PLAYER", 8, 1, 60);
        const int win = EnvBoundedInt("GAMEMESH_NAME_QUERY_WINDOW_SEC", 10, 1, 120);
        if (!SessionStore::Instance().Available() ||
            !SessionStore::Instance().ConsumeNameQueryQuota(querier, limit, win))
            return fail("ERR_RATE_LIMITED", "name query rate limited");
#endif
    } else if (target_id == 0) {
        return fail("ERR_INVALID_ARGUMENT", "target_player_id or player_name required");
    }
    game::PlayerBrief brief;
    std::string code, msg;
    if (!FillPublicBrief(target_id, name, &brief, &code, &msg))
        return fail(code.empty() ? "ERR_NOT_FOUND" : code.c_str(),
                    msg.empty() ? "not found" : msg.c_str());
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    *body->mutable_brief() = brief;
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool GameLogic::HandleQueryOnlineState(const game::QueryOnlineStateReq &req,
                                       game::GameResponse *rsp) {
    auto *body = rsp->mutable_query_online_state();
    auto fail = [&](const char *code, const char *msg) {
        body->set_ok(false);
        body->set_error_code(code);
        body->set_message(msg);
        rsp->set_ok(false);
        rsp->set_error_code(code);
        rsp->set_message(msg);
        return false;
    };
    if (req.player_id() == 0)
        return fail("ERR_UNAUTHENTICATED", "querier required");
    const uint64_t target = req.target_player_id() != 0 ? req.target_player_id() : req.player_id();
    std::string state = "offline";
#ifdef WEBSERVER_ENABLE_REDIS
    SessionStore::PublicPresence pres;
    if (!SessionStore::Instance().QueryPublicPresence(target, &pres))
        return fail("ERR_DEPENDENCY_UNAVAILABLE", "session unavailable");
    state = pres.state;
    body->set_map_instance_id(pres.map_instance_id);
    body->set_gamelogic_instance_id(pres.gamelogic_instance_id);
    if (pres.map_instance_id != 0 && PlacementStore::Instance().Available()) {
        PlacementRecord rec;
        if (PlacementStore::Instance().Get(pres.map_instance_id, &rec)) {
            body->set_line_no(rec.line_no);
            body->set_map_template_id(rec.map_template_id);
            body->set_kind(rec.kind);
            if (body->gamelogic_instance_id().empty())
                body->set_gamelogic_instance_id(rec.owner_logic_server_id);
        }
    }
#else
    (void)target;
    return fail("ERR_DEPENDENCY_UNAVAILABLE", "session unavailable");
#endif
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    body->set_player_id(target);
    body->set_state(state);
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool GameLogic::HandleGetSelfProfile(const game::GetSelfProfileReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_get_self_profile();
    game::PlayerAttributes attrs;
    if (!GetPlayerAttributes(req.player_id(), &attrs)) {
        std::string err;
        if (!LoadProfileUnlocked(req.player_id(), &attrs, &err)) {
            body->set_ok(false);
            body->set_error_code("ERR_PROFILE_NOT_FOUND");
            body->set_message(err.empty() ? "profile not loaded" : err);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        profiles_[req.player_id()] = attrs;
    }
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    *body->mutable_profile() = attrs;
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

bool GameLogic::HandleMove(const game::MoveReq &req, game::GameResponse *rsp) {
    const auto move_t0 = std::chrono::steady_clock::now();
    struct MoveSpan {
        uint64_t player = 0;
        std::chrono::steady_clock::time_point t0;
        ~MoveSpan() {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            NotePktStage("move", "rpc", ms, 0, player, 0, 0, 0);
        }
    } move_span;
    move_span.player = req.player_id();
    move_span.t0 = move_t0;
    auto *body = rsp->mutable_move();
    body->set_ok(false);
    if (req.player_id() == 0 || req.map_instance_id() == 0 || !req.has_position()) {
        body->set_error_code("ERR_INVALID_ARG");
        body->set_message("invalid move");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    ForwardRouteMeta meta_snap;
    bool have_meta = false;
    if (const ForwardRouteMeta *meta = ForwardMetaContext::Get()) {
        meta_snap = *meta;
        have_meta = true;
    }
    if (have_meta && meta_snap.map_instance_id != 0 &&
        meta_snap.map_instance_id != req.map_instance_id()) {
        body->set_error_code("ERR_MAP_MISMATCH");
        body->set_message("map_instance_id mismatch");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    if (have_meta && meta_snap.owner_epoch != 0 &&
        !MapInstanceRegistry::Instance().AcceptWrite(req.map_instance_id(), meta_snap.owner_epoch)) {
        body->set_error_code("ERR_STALE_EPOCH");
        body->set_message("ERR_STALE_EPOCH");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    if (!MapInstanceRegistry::Instance().PlayerOnMap(req.map_instance_id(), req.player_id()) &&
        !MapRuntime::Instance().HasPlayer(req.map_instance_id(), req.player_id())) {
        body->set_error_code(gameproto::kErrMapNotLoaded);
        body->set_message("map not loaded");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (LifeStateOf(req.player_id()) == "DEAD" || LifeStateOf(req.player_id()) == "RESPAWNING") {
            body->set_error_code("ERR_PLAYER_DEAD");
            body->set_message("ERR_PLAYER_DEAD");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        auto pit = profiles_.find(req.player_id());
        if (pit != profiles_.end() && pit->second.hp() <= 0) {
            pit->second.set_life_state("DEAD");
            body->set_error_code("ERR_PLAYER_DEAD");
            body->set_message("ERR_PLAYER_DEAD");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
    }
    MapEntity confirmed;
    AoiPushBatch pushes;
    std::string code;
    const MapMoveReject st = MapRuntime::Instance().Move(
        req.map_instance_id(), req.player_id(), req.position().x(), req.position().y(),
        req.position().z(), req.yaw(), rsp->seq(), NowMs(), &confirmed, &pushes, &code, false,
        req.client_time_ms());
    if (st != MapMoveReject::Ok) {
        body->set_error_code(code.empty() ? "ERR_MOVE_REJECTED" : code);
        body->set_message(body->error_code());
        // 拒绝时仍回写权威坐标，避免客户端本地飞远后再被大跳拉回。
        MapEntity cur;
        std::vector<MapEntity> ignore_aoi;
        uint64_t mid = 0;
        if (MapRuntime::Instance().SnapshotView(req.player_id(), &cur, &ignore_aoi, &mid) &&
            mid == req.map_instance_id()) {
            body->mutable_position()->set_x(cur.x);
            body->mutable_position()->set_y(cur.y);
            body->mutable_position()->set_z(cur.z);
            body->set_yaw(cur.yaw);
            body->set_state_seq(cur.state_seq);
            body->set_server_time_ms(NowMs());
        }
        rsp->set_ok(false);
        rsp->set_message(body->message());
        LOG_INFO << "move player_id=" << req.player_id() << " req=" << req.position().x() << ","
                 << req.position().y() << "," << req.position().z() << " accepted="
                 << (body->has_position() ? body->position().x() : 0) << ","
                 << (body->has_position() ? body->position().y() : 0) << ","
                 << (body->has_position() ? body->position().z() : 0)
                 << " error_code=" << body->error_code();
        return false;
    }
    EmitAoi(pushes);
    MaybePersistLastSafe(req.player_id(), confirmed, req.map_instance_id(), false);
    body->set_ok(true);
    body->clear_error_code();
    body->clear_message();
    body->mutable_position()->set_x(confirmed.x);
    body->mutable_position()->set_y(confirmed.y);
    body->mutable_position()->set_z(confirmed.z);
    body->set_yaw(confirmed.yaw);
    body->set_state_seq(confirmed.state_seq);
    body->set_server_time_ms(NowMs());
    rsp->set_ok(true);
    rsp->set_message("ok");
    const float adx = confirmed.x - req.position().x();
    const float ady = confirmed.y - req.position().y();
    const float adz = confirmed.z - req.position().z();
    if (adx * adx + ady * ady + adz * adz > 0.05f * 0.05f) {
        LOG_INFO << "move player_id=" << req.player_id() << " req=" << req.position().x() << ","
                 << req.position().y() << "," << req.position().z() << " accepted=" << confirmed.x
                 << "," << confirmed.y << "," << confirmed.z << " error_code=OK";
    }
    return true;
}

bool GameLogic::HandleWorldSnapshot(const game::WorldSnapshotReq &req, game::GameResponse *rsp) {
    auto *fs = rsp->mutable_full_snapshot();
    uint64_t baseline = 0;
#ifdef WEBSERVER_ENABLE_REDIS
    if (const ForwardRouteMeta *m = ForwardMetaContext::Get()) {
        if (PushReplayStore::Instance().Available() && !m->session_id.empty())
            baseline = PushReplayStore::Instance().CurrentSeq(req.player_id(), m->session_id);
    }
#else
    (void)req;
#endif
    if (req.last_applied_server_seq() > 0 && baseline == 0)
        baseline = req.last_applied_server_seq();
    if (!BuildFullStateSnapshot(req.player_id(), baseline, fs) || !fs->ok()) {
        const std::string code = fs->error_code().empty() ? "ERR_INTERNAL" : fs->error_code();
        const std::string msg = fs->message().empty() ? "snapshot failed" : fs->message();
        rsp->clear_full_snapshot();
        rsp->set_ok(false);
        rsp->set_error_code(code);
        rsp->set_message(msg);
        return false;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (fs->baseline_server_seq() > 0) {
        if (const ForwardRouteMeta *m = ForwardMetaContext::Get()) {
            if (PushReplayStore::Instance().Available() && !m->session_id.empty() &&
                !PushReplayStore::Instance().CommitSnapshot(req.player_id(), m->session_id,
                                                            fs->baseline_server_seq())) {
                LOG_WARN << "WorldSnapshot commit baseline failed player_id=" << req.player_id()
                         << " baseline=" << fs->baseline_server_seq();
            }
        }
    }
#endif
    rsp->set_ok(true);
    rsp->set_error_code("OK");
    rsp->set_message("ok");
    return true;
}

bool GameLogic::HandleRespawn(const game::RespawnReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_respawn();
    body->set_ok(false);
    if (req.player_id() == 0) {
        body->set_error_code("ERR_INVALID_ARGUMENT");
        rsp->set_ok(false);
        rsp->set_message(body->error_code());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!req.operation_id().empty() && last_safe_[req.player_id()].respawn_op == req.operation_id()) {
            body->set_ok(true);
            body->set_error_code("OK");
            body->set_message("idempotent");
            body->set_life_state(LifeStateOf(req.player_id()));
            rsp->set_ok(true);
            rsp->set_message("ok");
            return true;
        }
        auto &p = profiles_[req.player_id()];
        p.set_player_id(req.player_id());
        p.set_life_state("RESPAWNING");
    }
    const uint64_t mid = req.map_instance_id() != 0 ? req.map_instance_id()
                                                    : MapRuntime::Instance().PlayerMap(req.player_id());
    const uint64_t tpl = MapInstanceRegistry::Instance().TemplateId(mid);
    auto static_data = MapCatalog::Instance().Get(tpl);
    float x = 0, y = 0, z = 0, yaw = 0;
    if (static_data) {
        const auto &sp = static_data->default_spawn();
        x = sp.position.x;
        y = sp.position.y;
        z = sp.position.z;
        yaw = sp.yaw;
        ApplyTemplateSpawn(tpl, &x, &y, &z);
    }
    MapEntity confirmed;
    AoiPushBatch pushes;
    std::string code;
    if (mid != 0 && MapRuntime::Instance().HasPlayer(mid, req.player_id())) {
        const MapMoveReject st = MapRuntime::Instance().Move(
            mid, req.player_id(), x, y, z, yaw, rsp->seq(), NowMs(), &confirmed, &pushes, &code,
            true);
        if (st != MapMoveReject::Ok) {
            std::lock_guard<std::mutex> lk(mu_);
            profiles_[req.player_id()].set_life_state("DEAD");
            body->set_error_code(code.empty() ? "ERR_MOVE_REJECTED" : code);
            rsp->set_ok(false);
            rsp->set_message(body->error_code());
            return false;
        }
        EmitAoi(pushes);
        FillEntitySnapshot(confirmed, body->mutable_self());
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto &p = profiles_[req.player_id()];
        p.set_hp(p.max_hp() > 0 ? p.max_hp() : 100);
        p.set_mp(p.max_mp() > 0 ? p.max_mp() : 100);
        p.set_life_state("ALIVE");
        if (!req.operation_id().empty())
            last_safe_[req.player_id()].respawn_op = req.operation_id();
        body->set_life_state("ALIVE");
    }
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    rsp->set_ok(true);
    rsp->set_error_code("OK");
    rsp->set_message("ok");
    return true;
}

/**
 * GameService::HandleFrame 解析出 GameRequest 后的统一入口。
 * 根据 req.body_case()（protobuf oneof）路由到具体 Handler；
 * 游戏类接口（consume / skill / grant）会先 RequireSessionToken 校验 session_token。
 */
void GameLogic::SetFriendCommandsEnabled(bool enabled) { friend_commands_enabled_ = enabled; }

bool GameLogic::Handle(const game::GameRequest &req, game::GameResponse *rsp) {
    struct LogicHandleTimer {
        std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
        ~LogicHandleTimer() {
            const auto t1 = std::chrono::steady_clock::now();
            LogicMetrics::RecordHandle(
                std::chrono::duration<double>(t1 - t0).count());
        }
    } timer;

    if (!rsp)
        return false;
    rsp->Clear();
    rsp->set_seq(req.seq());  // 与请求 seq 对齐，便于客户端匹配异步响应
#ifdef WEBSERVER_ENABLE_MYSQL
    // Redis/Session 不可用时拒绝邮件。只有非正式环境显式 GAMEMESH_MAIL_DEV_SKIP_SESSION=1 才放行。
    auto mail_session = [&](uint64_t player_id) -> bool {
        if (!MailDevSkipSession()) {
#if defined(WEBSERVER_ENABLE_BRPC)
            if (!SessionRpcClient::Instance().ready()) {
#endif
#if defined(WEBSERVER_ENABLE_REDIS)
                if (!SessionStore::Instance().Available()) {
                    rsp->set_ok(false);
                    rsp->set_message("session unavailable");
                    return false;
                }
#else
                rsp->set_ok(false);
                rsp->set_message("session unavailable");
                return false;
#endif
#if defined(WEBSERVER_ENABLE_BRPC)
            }
#endif
        }
        return RequireSessionToken(req, player_id, rsp);
    };
#endif
    switch (req.body_case()) {
    case game::GameRequest::kFriendList:
    case game::GameRequest::kFriendSearch:
    case game::GameRequest::kFriendApply:
    case game::GameRequest::kFriendAccept:
    case game::GameRequest::kFriendReject:
    case game::GameRequest::kFriendDelete:
    case game::GameRequest::kFriendRequestList:
    case game::GameRequest::kFriendBlock:
    case game::GameRequest::kFriendUnblock:
    case game::GameRequest::kFriendBlockList:
        if (!friend_commands_enabled_) {
            LOG_WARN << "[friend] gamelogic rejected friend command seq=" << req.seq();
            rsp->set_ok(false);
            rsp->set_error_code("ERR_WRONG_ROUTE");
            rsp->set_message("friend commands are served by world");
            return false;
        }
        break;
    default:
        break;
    }
    switch (req.body_case()) {
        case game::GameRequest::kLogin:
            return HandleLogin(req.login(), rsp);
        case game::GameRequest::kReconnect:
            return HandleReconnect(req.reconnect(), rsp);
        case game::GameRequest::kValidateSession:
            return HandleValidateSession(req.validate_session(), rsp);
        case game::GameRequest::kCheckOnline:
            return HandleCheckOnline(req.check_online(), rsp);
        case game::GameRequest::kLogout:
            return HandleLogout(req.logout(), rsp);
        case game::GameRequest::kRegister:
            return HandleRegister(req.register_(), rsp);
        case game::GameRequest::kFlushBag:
            return HandleFlushBag(req.flush_bag(), rsp);
        case game::GameRequest::kConsumeItem:
            LOG_INFO << "[game] dispatch consume_item seq=" << req.seq();
            if (!RequireSessionToken(req, req.consume_item().player_id(), rsp)) {
                LOG_WARN << "[consume_item] session rejected seq=" << req.seq() << " msg="
                         << rsp->message();
                return false;
            }
#ifdef WEBSERVER_ENABLE_MYSQL
            PlayerItemPersistQueue::Instance().MarkOnline(req.consume_item().player_id());
#endif
            return HandleConsumeItem(req.consume_item(), rsp);
        case game::GameRequest::kReleaseSkill:
            LOG_INFO << "[game] dispatch release_skill seq=" << req.seq();
            if (!RequireSessionToken(req, req.release_skill().player_id(), rsp)) {
                LOG_WARN << "[release_skill] session rejected seq=" << req.seq() << " msg="
                         << rsp->message();
                return false;
            }
#ifdef WEBSERVER_ENABLE_MYSQL
            PlayerItemPersistQueue::Instance().MarkOnline(req.release_skill().player_id());
#endif
            return HandleReleaseSkill(req.release_skill(), rsp);
        case game::GameRequest::kGrantItem:
            LOG_INFO << "[game] dispatch grant_item seq=" << req.seq();
            if (!RequireSessionToken(req, req.grant_item().player_id(), rsp)) {
                LOG_WARN << "[grant_item] session rejected seq=" << req.seq() << " msg="
                         << rsp->message();
                return false;
            }
            return HandleGrantItem(req.grant_item(), rsp);
        case game::GameRequest::kEnterMap:
            if (!RequireSessionToken(req, req.enter_map().player_id(), rsp))
                return false;
            {
                const bool ok = HandleEnterMap(req.enter_map(), rsp);
                if (!ok && rsp->has_enter_map() && rsp->enter_map().error_code().empty()) {
                    const std::string &m = rsp->enter_map().message();
                    if (m.size() >= 4 && m.compare(0, 4, "ERR_") == 0)
                        rsp->mutable_enter_map()->set_error_code(m);
                }
                return ok;
            }
        case game::GameRequest::kLeaveMap:
            if (!RequireSessionToken(req, req.leave_map().player_id(), rsp))
                return false;
            return HandleLeaveMap(req.leave_map(), rsp);
        case game::GameRequest::kMapPing:
            if (!RequireSessionToken(req, req.map_ping().player_id(), rsp))
                return false;
            return HandleMapPing(req.map_ping(), rsp);
        case game::GameRequest::kInteractPortal:
            if (!RequireSessionToken(req, req.interact_portal().player_id(), rsp))
                return false;
            return HandleInteractPortal(req.interact_portal(), rsp);
        case game::GameRequest::kChatSend:
            if (!RequireSessionToken(req, req.chat_send().player_id(), rsp))
                return false;
            return HandleChatSend(req.chat_send(), rsp);
        case game::GameRequest::kFriendList:
            if (!RequireSessionToken(req, req.friend_list().player_id(), rsp))
                return false;
            return HandleFriendList(req.friend_list(), rsp);
        case game::GameRequest::kFriendSearch:
            if (!RequireSessionToken(req, req.friend_search().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleSearch(req.friend_search(), rsp);
        case game::GameRequest::kFriendApply:
            if (!RequireSessionToken(req, req.friend_apply().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleApply(req.friend_apply(), rsp);
        case game::GameRequest::kFriendAccept:
            if (!RequireSessionToken(req, req.friend_accept().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleAccept(req.friend_accept(), rsp);
        case game::GameRequest::kFriendReject:
            if (!RequireSessionToken(req, req.friend_reject().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleReject(req.friend_reject(), rsp);
        case game::GameRequest::kFriendDelete:
            if (!RequireSessionToken(req, req.friend_delete().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleDelete(req.friend_delete(), rsp);
        case game::GameRequest::kFriendRequestList:
            if (!RequireSessionToken(req, req.friend_request_list().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleRequestList(req.friend_request_list(), rsp);
        case game::GameRequest::kFriendBlock:
            if (!RequireSessionToken(req, req.friend_block().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleBlock(req.friend_block(), rsp);
        case game::GameRequest::kFriendUnblock:
            if (!RequireSessionToken(req, req.friend_unblock().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleUnblock(req.friend_unblock(), rsp);
        case game::GameRequest::kFriendBlockList:
            if (!RequireSessionToken(req, req.friend_block_list().player_id(), rsp))
                return false;
            return FriendService::Instance().HandleBlockList(req.friend_block_list(), rsp);
        case game::GameRequest::kGetPlayerBrief:
            if (!RequireSessionToken(req, req.get_player_brief().player_id(), rsp))
                return false;
            return HandleGetPlayerBrief(req.get_player_brief(), rsp);
        case game::GameRequest::kQueryOnlineState:
            if (!RequireSessionToken(req, req.query_online_state().player_id(), rsp))
                return false;
            return HandleQueryOnlineState(req.query_online_state(), rsp);
        case game::GameRequest::kGetSelfProfile:
            if (!RequireSessionToken(req, req.get_self_profile().player_id(), rsp))
                return false;
            return HandleGetSelfProfile(req.get_self_profile(), rsp);
        case game::GameRequest::kMove:
            if (!RequireSessionToken(req, req.move().player_id(), rsp))
                return false;
            return HandleMove(req.move(), rsp);
        case game::GameRequest::kWorldSnapshot:
            if (!RequireSessionToken(req, req.world_snapshot().player_id(), rsp)) {
                // 会话已断开、人还在这张图上：客户端会先拉全量快照，不要再回 ERR_INTERNAL。
                const bool still_on_map =
                    MapRuntime::Instance().PlayerMap(req.world_snapshot().player_id()) != 0;
                if (!(still_on_map && rsp->error_code() == gameproto::kErrFenceStale))
                    return false;
                const uint64_t seq = req.seq();
                rsp->Clear();
                rsp->set_seq(seq);
                LOG_INFO << "world snapshot while session disconnected player="
                         << req.world_snapshot().player_id();
            }
            return HandleWorldSnapshot(req.world_snapshot(), rsp);
        case game::GameRequest::kRespawn:
            if (!RequireSessionToken(req, req.respawn().player_id(), rsp))
                return false;
            return HandleRespawn(req.respawn(), rsp);
#ifdef WEBSERVER_ENABLE_MYSQL
        case game::GameRequest::kMailboxSummary:
            if (!mail_session(req.mailbox_summary().player_id()))
                return false;
            return MailService::Instance().HandleMailboxSummary(req.mailbox_summary(), rsp);
        case game::GameRequest::kMailList:
            if (!mail_session(req.mail_list().player_id()))
                return false;
            return MailService::Instance().HandleMailList(req.mail_list(), rsp);
        case game::GameRequest::kMailGet:
            if (!mail_session(req.mail_get().player_id()))
                return false;
            return MailService::Instance().HandleMailGet(req.mail_get(), rsp);
        case game::GameRequest::kMailRead:
            if (!mail_session(req.mail_read().player_id()))
                return false;
            return MailService::Instance().HandleMailRead(req.mail_read(), rsp);
        case game::GameRequest::kMailClaim:
            if (!mail_session(req.mail_claim().player_id()))
                return false;
            return MailService::Instance().HandleMailClaim(req.mail_claim(), rsp);
        case game::GameRequest::kMailBatchClaim:
            if (!mail_session(req.mail_batch_claim().player_id()))
                return false;
            return MailService::Instance().HandleMailBatchClaim(req.mail_batch_claim(), rsp);
        case game::GameRequest::kMailFavorite:
            if (!mail_session(req.mail_favorite().player_id()))
                return false;
            return MailService::Instance().HandleMailFavorite(req.mail_favorite(), rsp);
        case game::GameRequest::kMailBatchRead:
            if (!mail_session(req.mail_batch_read().player_id()))
                return false;
            return MailService::Instance().HandleMailBatchRead(req.mail_batch_read(), rsp);
        case game::GameRequest::kMailBatchDelete:
            if (!mail_session(req.mail_batch_delete().player_id()))
                return false;
            return MailService::Instance().HandleMailBatchDelete(req.mail_batch_delete(), rsp);
        case game::GameRequest::kPlayerMailSend:
            if (!mail_session(req.player_mail_send().sender_player_id()))
                return false;
            return MailService::Instance().HandlePlayerMailSend(req.player_mail_send(), rsp);
        case game::GameRequest::kMailDeliver:
            // TCP 投递仅作联调；Formal/默认拒绝（系统投递走 MailService::Deliver / 内部 brpc）
            if (!AllowUnsafeDebugCommandsEnv()) {
                rsp->set_ok(false);
                rsp->set_message("ERR_COMMAND_FORBIDDEN");
                return false;
            }
            if (!RequireSessionToken(req, req.mail_deliver().receiver_id(), rsp))
                return false;
            return MailService::Instance().HandleMailDeliver(req.mail_deliver(), rsp);
#endif
        default:
            rsp->set_ok(false);
            rsp->set_message("unknown or empty request body");
            return false;
    }
}
