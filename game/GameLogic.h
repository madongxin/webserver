#pragma once

#include "game.pb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

class GameLogic {
public:
    static GameLogic &Instance();
    bool Handle(const game::GameRequest &req, game::GameResponse *rsp);

private:
    GameLogic() = default;
    bool HandleConsumeItem(const game::ConsumeItemReq &req, game::GameResponse *rsp);
    bool HandleReleaseSkill(const game::ReleaseSkillReq &req, game::GameResponse *rsp);
    bool HandleGrantItem(const game::GrantItemReq &req, game::GameResponse *rsp);
    bool HandleLogin(const game::LoginReq &req, game::GameResponse *rsp);
    bool HandleValidateSession(const game::ValidateSessionReq &req, game::GameResponse *rsp);
    bool HandleCheckOnline(const game::CheckOnlineReq &req, game::GameResponse *rsp);
    bool HandleLogout(const game::LogoutReq &req, game::GameResponse *rsp);
    bool RequireSessionToken(const game::GameRequest &req, uint64_t player_id, game::GameResponse *rsp);
    void EnsurePlayer(uint64_t player_id);

    std::mutex mu_;
    std::map<uint64_t, std::map<uint32_t, uint32_t>> inventory_;
    std::map<uint64_t, std::map<uint32_t, int64_t>> skill_cd_until_ms_;
};
