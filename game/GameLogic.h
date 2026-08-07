#pragma once

#include "game.pb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

/** 游戏业务单例：由 GameService::HandleFrame 在反序列化 GameRequest 后调用 */
class GameLogic {
public:
    static GameLogic &Instance();
    /** 按 oneof body 分发到各 HandleXxx；填充 GameResponse（含 seq） */
    bool Handle(const game::GameRequest &req, game::GameResponse *rsp);

    /** 邮件领取等：查询内存背包聚合数量 */
    uint32_t GetItemCount(uint64_t player_id, uint32_t item_id);
    /** 提交 GameDB 前拷贝背包快照（供 worker 软上限校验） */
    void CopyInventory(uint64_t player_id, std::unordered_map<uint32_t, uint32_t> *out);
    /** 邮件领取事务提交后同步增加内存背包 */
    bool ApplyItemReward(uint64_t player_id, uint32_t item_id, uint32_t count);
    /** 事务失败时回退内存（尽力而为） */
    bool RollbackItemReward(uint64_t player_id, uint32_t item_id, uint32_t count);

    /** Gateway BindPlayer：已认证玩家加载内存态（不接收凭证） */
    void BindAuthenticatedPlayer(uint64_t player_id);
    bool FlushBag(uint64_t player_id, const std::string &reason);

private:
    GameLogic() = default;
    bool HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp);
    bool HandleReleaseSkill(const game::ReleaseSkillReq &req, game::GameResponse *rsp);
    /** 发放道具：内存即时生效，MySQL 经 PlayerItemPersistQueue 异步落库 */
    bool HandleGrantItem(const game::GrantItemReq &req, game::GameResponse *rsp);
    bool HandleLogin(const game::LoginReq &req, game::GameResponse *rsp);
    bool HandleReconnect(const game::ReconnectReq &req, game::GameResponse *rsp);
    bool HandleValidateSession(const game::ValidateSessionReq &req, game::GameResponse *rsp);
    bool HandleCheckOnline(const game::CheckOnlineReq &req, game::GameResponse *rsp);
    bool HandleLogout(const game::LogoutReq &req, game::GameResponse *rsp);
    bool HandleRegister(const game::RegisterReq &req, game::GameResponse *rsp);
    bool HandleFlushBag(const game::FlushBagReq &req, game::GameResponse *rsp);
    bool HandleEnterMap(const game::EnterMapReq &req, game::GameResponse *rsp);
    bool HandleLeaveMap(const game::LeaveMapReq &req, game::GameResponse *rsp);
    bool HandleMapPing(const game::MapPingReq &req, game::GameResponse *rsp);
    bool HandleChatSend(const game::ChatSendReq &req, game::GameResponse *rsp);
    bool HandleFriendList(const game::FriendListReq &req, game::GameResponse *rsp);
    bool RequireSessionToken(const game::GameRequest &req, uint64_t player_id, game::GameResponse *rsp);
    void EnsurePlayer(uint64_t player_id);

    std::mutex mu_;
    /** 内存背包：player_id -> (item_config_id -> 聚合数量)，与 consume_item 共用 */
    std::map<uint64_t, std::map<uint32_t, uint32_t>> inventory_;
    std::map<uint64_t, std::map<uint32_t, int64_t>> skill_cd_until_ms_;
};
