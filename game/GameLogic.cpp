#include "GameLogic.h"

#include "FormalMode.h"
#include "ForwardMetaContext.h"
#include "LogicMetrics.h"
#include "MapCatalog.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#include "MapRuntime.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementAuthority.h"
#include "PlacementStore.h"
#include "SessionStore.h"
#endif
#ifdef WEBSERVER_ENABLE_BRPC
#include "BrpcGameDbRepository.h"
#include "GameLogicPush.h"
#include "GameLogicServiceImpl.h"
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

#include <chrono>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef WEBSERVER_ENABLE_REDIS
bool FetchAuthorityPlacement(uint64_t map_instance_id, PlacementRecord *out, std::string *err) {
    if (!out || map_instance_id == 0) {
        if (err)
            *err = "ERR_INVALID_MAP";
        return false;
    }
    if (PlacementStore::Instance().Available() &&
        PlacementStore::Instance().Get(map_instance_id, out))
        return true;
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

#ifdef WEBSERVER_ENABLE_BRPC
void PublishAoiBatch(const AoiPushBatch &batch) {
    if (batch.events.empty())
        return;
    std::unordered_map<uint64_t, game::AoiDelta> by_player;
    std::unordered_map<uint64_t, std::pair<std::string, std::string>> route;
    for (const auto &ev : batch.events) {
        auto &delta = by_player[ev.recipient_id];
        delta.set_map_instance_id(batch.map_instance_id);
        auto *pe = delta.add_events();
        pe->set_op(ev.op);
        FillEntitySnapshot(ev.snapshot, pe->mutable_entity());
        if (route[ev.recipient_id].first.empty())
            route[ev.recipient_id] = {ev.gateway_instance_id, ev.session_id};
    }
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
        std::string payload;
        if (!notify.SerializeToString(&payload))
            continue;
        const auto &rt = route[kv.first];
        const bool reliable = has_el;
        const bool coalescable = has_move && !has_el;
        const bool pok = GameLogicPush::PushToBoundGateway(
            rt.first, kv.first, rt.second, "aoi.delta.v1", payload, reliable, coalescable, 0);
        LOG_INFO << "PublishAoiBatch player=" << kv.first << " gw=" << rt.first
                 << " sid=" << rt.second << " ok=" << pok
                 << " events=" << kv.second.events_size() << " reliable=" << reliable;
    }
}
#endif

}  // namespace

GameLogic &GameLogic::Instance() {
    static GameLogic g;
    return g;
}

void GameLogic::EmitAoi(const AoiPushBatch &batch) {
#ifdef WEBSERVER_ENABLE_BRPC
    if (batch.events.empty())
        return;
    if (RpcOffloadPool::Instance().started() &&
        RpcOffloadPool::Instance().TryPost([batch]() { PublishAoiBatch(batch); }))
        return;
    PublishAoiBatch(batch);
#else
    (void)batch;
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
        } else if (!apply_blocked_for_test_ && committed_asset_version == cur + 1) {
            for (const auto &g : grants) {
                if (g.count == 0)
                    continue;
                inventory_[player_id][static_cast<uint32_t>(g.asset_id)] += g.count;
            }
            asset_version_[player_id] = committed_asset_version;
            asset_dirty_.erase(player_id);
            return true;
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
    if (!out || player_id == 0)
        return false;
    out->Clear();
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    out->set_ok(true);
    out->set_message("ok");
    out->set_player_id(player_id);
    out->set_asset_version(asset_version_[player_id]);
    for (const auto &kv : inventory_[player_id]) {
        if (kv.second == 0)
            continue;
        out->add_item_ids(kv.first);
        out->add_item_counts(kv.second);
    }
    return true;
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
            rsp->set_ok(false);
            rsp->set_message(err.empty() ? "session invalid" : err);
            return false;
        }
        return true;
    }
#endif
    if (!SessionStore::Instance().Available())
        return true;
    if (!SessionStore::Instance().ValidateToken(player_id, req.session_token(), &err)) {
        rsp->set_ok(false);
        rsp->set_message(err);
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
            if (FetchAuthorityPlacement(place.map_instance_id, &prec, &ignore))
                place.lease_until_unix = prec.lease_until;
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
    MapInstanceRegistry::Instance().AddPlayer(place.map_instance_id, req.player_id());

    auto rollback_enter = [&]() {
        MapInstanceRegistry::Instance().RemovePlayer(place.map_instance_id, req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
        PlacementStore::Instance().ReleaseByPlayer(req.player_id());
#endif
    };

    std::shared_ptr<const MapStaticData> static_data =
        MapCatalog::Instance().Get(place.map_template_id);
    if (static_data) {
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

    if (static_data) {
        const MapSpawnPoint &sp = static_data->default_spawn();
        body->mutable_spawn_position()->set_x(sp.position.x);
        body->mutable_spawn_position()->set_y(sp.position.y);
        body->mutable_spawn_position()->set_z(sp.position.z);
        body->set_spawn_yaw(sp.yaw);
        body->set_map_data_version(static_data->data_version());
        body->set_map_data_sha256(static_data->sha256());

        game::PlayerAttributes attrs;
        GetPlayerAttributes(req.player_id(), &attrs);
        MapEntity me;
        me.player_id = req.player_id();
        me.player_name = attrs.player_name();
        me.x = sp.position.x;
        me.y = sp.position.y;
        me.z = sp.position.z;
        me.yaw = sp.yaw;
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
                                          &pushes, &rerr)) {
            rollback_enter();
            body->set_ok(false);
            body->set_message(rerr.empty() ? "map runtime enter failed" : rerr);
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        FillEntitySnapshot(self, body->mutable_self());
        for (const auto &e : snap)
            FillEntitySnapshot(e, body->add_aoi_snapshot());
        EmitAoi(pushes);
    }
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementStore::Instance().ConfirmSlot(req.player_id(), place.map_instance_id);
#endif
    rsp->set_ok(true);
    rsp->set_message("entered");
    LOG_INFO << "[enter_map] player=" << req.player_id() << " map=" << place.map_instance_id
             << " epoch=" << place.owner_epoch
             << " gl=" << MapInstanceRegistry::Instance().local_instance_id();
#if defined(WEBSERVER_ENABLE_BRPC)
    // 真实业务可靠 Push：进图结果按 gateway_instance_id 推送（非广播）
    {
        game::GameResponse notify;
        notify.set_ok(true);
        notify.set_message("enter_map_notify");
        *notify.mutable_enter_map() = *body;
        std::string payload;
        if (notify.SerializeToString(&payload)) {
            const bool pushed = GameLogicPush::PushToBoundGateway(
                "", req.player_id(), "", "enter_map_notify", payload, true, false, 0);
            LOG_INFO << "[enter_map] PushToBoundGateway player=" << req.player_id()
                     << " ok=" << pushed;
        }
    }
#endif
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
    MapRuntime::Instance().Leave(req.map_instance_id(), req.player_id(), &pushes);
    MapInstanceRegistry::Instance().RemovePlayer(req.map_instance_id(), req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
    PlacementStore::Instance().ReleaseByPlayer(req.player_id());
#endif
    EmitAoi(pushes);
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
    (void)req;
    auto *body = rsp->mutable_chat_send();
    body->set_ok(false);
    body->set_error_code("NOT_IMPLEMENTED");
    body->set_message("chat stub: world module boundary only");
    rsp->set_ok(false);
    rsp->set_message(body->message());
    return false;
}

bool GameLogic::HandleFriendList(const game::FriendListReq &req, game::GameResponse *rsp) {
    (void)req;
    auto *body = rsp->mutable_friend_list();
    body->set_ok(false);
    body->set_error_code("NOT_IMPLEMENTED");
    body->set_message("friend stub: world module boundary only");
    rsp->set_ok(false);
    rsp->set_message(body->message());
    return false;
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
        body->set_error_code("ERR_NOT_ON_MAP");
        body->set_message("not on map");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    MapEntity confirmed;
    AoiPushBatch pushes;
    std::string code;
    const MapMoveReject st = MapRuntime::Instance().Move(
        req.map_instance_id(), req.player_id(), req.position().x(), req.position().y(),
        req.position().z(), req.yaw(), rsp->seq(), NowMs(), &confirmed, &pushes, &code);
    if (st != MapMoveReject::Ok) {
        body->set_error_code(code.empty() ? "ERR_MOVE_REJECTED" : code);
        body->set_message(body->error_code());
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    EmitAoi(pushes);
    body->set_ok(true);
    body->set_error_code("OK");
    body->set_message("ok");
    body->mutable_position()->set_x(confirmed.x);
    body->mutable_position()->set_y(confirmed.y);
    body->mutable_position()->set_z(confirmed.z);
    body->set_yaw(confirmed.yaw);
    body->set_state_seq(confirmed.state_seq);
    body->set_server_time_ms(NowMs());
    rsp->set_ok(true);
    rsp->set_message("ok");
    return true;
}

/**
 * GameService::HandleFrame 解析出 GameRequest 后的统一入口。
 * 根据 req.body_case()（protobuf oneof）路由到具体 Handler；
 * 游戏类接口（consume / skill / grant）会先 RequireSessionToken 校验 session_token。
 */
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
            return HandleEnterMap(req.enter_map(), rsp);
        case game::GameRequest::kLeaveMap:
            if (!RequireSessionToken(req, req.leave_map().player_id(), rsp))
                return false;
            return HandleLeaveMap(req.leave_map(), rsp);
        case game::GameRequest::kMapPing:
            if (!RequireSessionToken(req, req.map_ping().player_id(), rsp))
                return false;
            return HandleMapPing(req.map_ping(), rsp);
        case game::GameRequest::kChatSend:
            if (!RequireSessionToken(req, req.chat_send().player_id(), rsp))
                return false;
            return HandleChatSend(req.chat_send(), rsp);
        case game::GameRequest::kFriendList:
            if (!RequireSessionToken(req, req.friend_list().player_id(), rsp))
                return false;
            return HandleFriendList(req.friend_list(), rsp);
        case game::GameRequest::kGetSelfProfile:
            if (!RequireSessionToken(req, req.get_self_profile().player_id(), rsp))
                return false;
            return HandleGetSelfProfile(req.get_self_profile(), rsp);
        case game::GameRequest::kMove:
            if (!RequireSessionToken(req, req.move().player_id(), rsp))
                return false;
            return HandleMove(req.move(), rsp);
#ifdef WEBSERVER_ENABLE_MYSQL
        case game::GameRequest::kMailboxSummary:
            if (!RequireSessionToken(req, req.mailbox_summary().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailboxSummary(req.mailbox_summary(), rsp);
        case game::GameRequest::kMailList:
            if (!RequireSessionToken(req, req.mail_list().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailList(req.mail_list(), rsp);
        case game::GameRequest::kMailGet:
            if (!RequireSessionToken(req, req.mail_get().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailGet(req.mail_get(), rsp);
        case game::GameRequest::kMailRead:
            if (!RequireSessionToken(req, req.mail_read().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailRead(req.mail_read(), rsp);
        case game::GameRequest::kMailClaim:
            if (!RequireSessionToken(req, req.mail_claim().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailClaim(req.mail_claim(), rsp);
        case game::GameRequest::kMailBatchClaim:
            if (!RequireSessionToken(req, req.mail_batch_claim().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailBatchClaim(req.mail_batch_claim(), rsp);
        case game::GameRequest::kMailFavorite:
            if (!RequireSessionToken(req, req.mail_favorite().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailFavorite(req.mail_favorite(), rsp);
        case game::GameRequest::kMailBatchRead:
            if (!RequireSessionToken(req, req.mail_batch_read().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailBatchRead(req.mail_batch_read(), rsp);
        case game::GameRequest::kMailBatchDelete:
            if (!RequireSessionToken(req, req.mail_batch_delete().player_id(), rsp))
                return false;
            return MailService::Instance().HandleMailBatchDelete(req.mail_batch_delete(), rsp);
        case game::GameRequest::kPlayerMailSend:
            if (!RequireSessionToken(req, req.player_mail_send().sender_player_id(), rsp))
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
