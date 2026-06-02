#pragma once

#include "game.pb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

/** 游戏业务单例：由 GameService::HandleFrame 在反序列化 GameRequest 后调用 */
class GameLogic {
public:
    static GameLogic &Instance();
    /** 按 oneof body 分发到各 HandleXxx；填充 GameResponse（含 seq） */
    bool Handle(const game::GameRequest &req, game::GameResponse *rsp);

private:
    GameLogic() = default;
    bool HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp);
    bool HandleReleaseSkill(const game::ReleaseSkillReq &req, game::GameResponse *rsp);
    /** 发放道具：内存即时生效，MySQL 经 PlayerItemPersistQueue 异步落库 */
    bool HandleGrantItem(const game::GrantItemReq &req, game::GameResponse *rsp);
    bool HandleLogin(const game::LoginReq &req, game::GameResponse *rsp);
    bool HandleValidateSession(const game::ValidateSessionReq &req, game::GameResponse *rsp);
    bool HandleCheckOnline(const game::CheckOnlineReq &req, game::GameResponse *rsp);
    bool HandleLogout(const game::LogoutReq &req, game::GameResponse *rsp);
    bool RequireSessionToken(const game::GameRequest &req, uint64_t player_id, game::GameResponse *rsp);
    void EnsurePlayer(uint64_t player_id);

    std::mutex mu_;
    /** 内存背包：player_id -> (item_config_id -> 聚合数量)，与 consume_item 共用 */
    std::map<uint64_t, std::map<uint32_t, uint32_t>> inventory_;
    std::map<uint64_t, std::map<uint32_t, int64_t>> skill_cd_until_ms_;
};
