#pragma once

#include "game.pb.h"

#include <cstdint>
#include <string>

struct SessionRecord {
    std::string token;
    uint32_t server_id = 0;
    int64_t login_time_sec = 0;
    std::string device_id;
};

class SessionStore {
public:
    static SessionStore &Instance();

    bool InitFromConfig();
    bool Available() const { return available_; }

    bool Login(const game::LoginReq &req, game::LoginRsp *rsp);
    bool Validate(const game::ValidateSessionReq &req, game::ValidateSessionRsp *rsp);
    bool CheckOnline(const game::CheckOnlineReq &req, game::CheckOnlineRsp *rsp);
    bool Logout(const game::LogoutReq &req, game::LogoutRsp *rsp);
    bool ValidateToken(uint64_t player_id, const std::string &token, std::string *err);
    bool IsPlayerOnline(uint64_t player_id) const;

private:
    SessionStore() = default;
    std::string SessionKey(uint64_t player_id) const;
    bool LoadSession(uint64_t player_id, SessionRecord *out);
    bool SaveSession(uint64_t player_id, const SessionRecord &rec, int ttl_sec);

    bool available_ = false;
    int default_ttl_sec_ = 7200;
    int long_ttl_sec_ = 86400;
};
