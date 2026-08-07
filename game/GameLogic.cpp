#include "GameLogic.h"

#include "ForwardMetaContext.h"
#include "LogicMetrics.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#ifdef WEBSERVER_ENABLE_BRPC
#include "SessionRpcClient.h"
#endif
#endif
#ifdef WEBSERVER_ENABLE_MYSQL
#include "ConnectionPool.h"
#include "MailService.h"
#include "PlayerAccountStore.h"
#include "PlayerItemPersistQueue.h"
#include "PlayerItemStore.h"
#endif

#include "Logging.h"

#include <chrono>
#include <sstream>

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

GameLogic &GameLogic::Instance() {
    static GameLogic g;
    return g;
}

void GameLogic::EnsurePlayer(uint64_t player_id) {
    if (inventory_.count(player_id) != 0) {
        if (skill_cd_until_ms_.count(player_id) == 0)
            skill_cd_until_ms_[player_id] = {};
        return;
    }
    auto &inv = inventory_[player_id];
#ifdef WEBSERVER_ENABLE_MYSQL
    std::map<uint32_t, uint32_t> loaded;
    if (PlayerItemStore::Instance().LoadInventoryAggregate(player_id, &loaded) && !loaded.empty()) {
        inv = std::move(loaded);
        LOG_INFO << "EnsurePlayer loaded bag from DB player_id=" << player_id
                 << " kinds=" << inv.size();
    } else {
        // 新号或无存档：空背包（注册后靠 grant/mail 获得）
        inv.clear();
    }
#else
    inv[1001] = 10;
    inv[1002] = 5;
#endif
    if (skill_cd_until_ms_.count(player_id) == 0)
        skill_cd_until_ms_[player_id] = {};
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

bool GameLogic::RollbackItemReward(uint64_t player_id, uint32_t item_id, uint32_t count) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
    auto &n = inventory_[player_id][item_id];
    if (n < count)
        n = 0;
    else
        n -= count;
    return true;
}

bool GameLogic::HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp) {
    LOG_INFO << "[consume_item] recv player_id=" << req.player_id() << " item_id=" << req.item_id()
             << " count=" << req.count();
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(req.player_id());
    auto &inv = inventory_[req.player_id()];
    if (req.count() == 0) {
        rsp->set_ok(false);
        rsp->set_message("count must be > 0");
        LOG_WARN << "[consume_item] reject player_id=" << req.player_id() << " reason=count_zero";
        return false;
    }
    if (inv[req.item_id()] < req.count()) {
        rsp->set_ok(false);
        std::ostringstream os;
        os << "not enough item " << req.item_id() << ", have " << inv[req.item_id()];
        rsp->set_message(os.str());
        auto *body = rsp->mutable_consume_item();
        body->set_ok(false);
        body->set_message(rsp->message());
        body->set_remain_count(inv[req.item_id()]);
        LOG_WARN << "[consume_item] fail player_id=" << req.player_id() << " item_id=" << req.item_id()
                 << " have=" << inv[req.item_id()] << " need=" << req.count();
        return false;
    }
    inv[req.item_id()] -= req.count();
    rsp->set_ok(true);
    rsp->set_message("item consumed");
    auto *body = rsp->mutable_consume_item();
    body->set_ok(true);
    body->set_message("ok");
    body->set_remain_count(inv[req.item_id()]);
    LOG_INFO << "[consume_item] ok player_id=" << req.player_id() << " item_id=" << req.item_id()
             << " remain=" << inv[req.item_id()];
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

    // 先改内存，保证客户端下一次 consume 能立刻读到新数量
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(req.player_id());
    auto &inv = inventory_[req.player_id()];
    inv[item_id] += req.count();

#ifdef WEBSERVER_ENABLE_MYSQL
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        rsp->set_ok(false);
        rsp->set_message("mysql pool not initialized");
        body->set_ok(false);
        body->set_message(rsp->message());
        return false;
    }
    // 异步落库：不阻塞 RPC 线程连接 MySQL
    PendingPlayerItem pending;
    pending.player_id = req.player_id();
    pending.item_id = req.item_id();
    pending.count = req.count();
    pending.expire_time_sec = req.expire_time_sec();
    pending.extra_data = req.extra_data();
    PlayerItemPersistQueue::Instance().Enqueue(pending);
    PlayerItemPersistQueue::Instance().MarkOnline(req.player_id());
#else
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
    body->set_bag_total(inv[item_id]);
    LOG_INFO << "[grant_item] ok player_id=" << req.player_id() << " item_id=" << item_id
             << " bag_total=" << inv[item_id] << " (db queued)";
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

void GameLogic::BindAuthenticatedPlayer(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    EnsurePlayer(player_id);
#ifdef WEBSERVER_ENABLE_MYSQL
    PlayerItemPersistQueue::Instance().MarkOnline(player_id);
#endif
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
        std::lock_guard<std::mutex> lk(mu_);
        inventory_.erase(req.player_id());
        skill_cd_until_ms_.erase(req.player_id());
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
#ifdef WEBSERVER_ENABLE_MYSQL
    auto *body = rsp->mutable_register_();
    uint64_t player_id = 0;
    std::string err;
    if (!PlayerAccountStore::Instance().Register(req.device_id(), req.display_name(), &player_id,
                                                 &err)) {
        body->set_ok(false);
        body->set_message(err.empty() ? "register failed" : err);
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    body->set_ok(true);
    body->set_message("ok");
    body->set_player_id(player_id);
    rsp->set_ok(true);
    rsp->set_message("ok");
    LOG_INFO << "Register ok player_id=" << player_id << " device=" << req.device_id();
    return true;
#else
    (void)req;
    rsp->set_ok(false);
    rsp->set_message("mysql not enabled");
    return false;
#endif
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
    const ForwardRouteMeta *meta = ForwardMetaContext::Get();
    if (meta && meta->map_instance_id != 0) {
        if (!meta->gamelogic_instance_id.empty() &&
            meta->gamelogic_instance_id != MapInstanceRegistry::Instance().local_instance_id()) {
            body->set_message("wrong logic owner");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        place.map_instance_id = meta->map_instance_id;
        place.owner_epoch = meta->owner_epoch;
        place.route_version = meta->route_version;
        place.owner_gamelogic_id = meta->gamelogic_instance_id;
        place.map_template_id = req.map_template_id();
        place.realm_id = req.realm_id();
    } else {
        if (MapPlacement::Instance().owners().empty())
            MapPlacement::Instance().ConfigureOwners({MapInstanceRegistry::Instance().local_instance_id()});
        if (!MapPlacement::Instance().ResolveOrAllocate(req.realm_id(), req.map_template_id(),
                                                       req.map_instance_id(), &place)) {
            body->set_message("placement failed");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
        // 单进程：只认本地 owner
        if (place.owner_gamelogic_id != MapInstanceRegistry::Instance().local_instance_id()) {
            body->set_message("owner not local");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
    }

    if (!MapInstanceRegistry::Instance().Claim(place.map_instance_id, place.map_template_id,
                                               place.owner_epoch)) {
        body->set_message("claim rejected");
        rsp->set_ok(false);
        rsp->set_message(body->message());
        return false;
    }
    MapInstanceRegistry::Instance().AddPlayer(place.map_instance_id, req.player_id());

    body->set_ok(true);
    body->set_message("entered");
    body->set_map_template_id(place.map_template_id);
    body->set_map_instance_id(place.map_instance_id);
    body->set_gamelogic_instance_id(MapInstanceRegistry::Instance().local_instance_id());
    body->set_owner_epoch(place.owner_epoch);
    body->set_route_version(place.route_version);
    rsp->set_ok(true);
    rsp->set_message("entered");
    LOG_INFO << "[enter_map] player=" << req.player_id() << " map=" << place.map_instance_id
             << " epoch=" << place.owner_epoch
             << " gl=" << MapInstanceRegistry::Instance().local_instance_id();
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
    MapInstanceRegistry::Instance().RemovePlayer(req.map_instance_id(), req.player_id());
    body->set_ok(true);
    body->set_message("left");
    rsp->set_ok(true);
    rsp->set_message("left");
    return true;
}

bool GameLogic::HandleMapPing(const game::MapPingReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_map_ping();
    body->set_ok(false);
    const ForwardRouteMeta *meta = ForwardMetaContext::Get();
    if (meta && meta->map_instance_id != 0) {
        if (!MapInstanceRegistry::Instance().AcceptWrite(meta->map_instance_id, meta->owner_epoch)) {
            body->set_message("stale_owner_epoch");
            rsp->set_ok(false);
            rsp->set_message(body->message());
            return false;
        }
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
        case game::GameRequest::kMailDeliver:
            // TCP 投递仅作联调：校验 session 属于 receiver；正式玩法请调 MailService::Deliver
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
