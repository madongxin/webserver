#pragma once

#include "game.pb.h"

#include <cstdint>
#include <string>
#include <vector>

/** 会话状态（Redis 字段 state） */
enum class SessionState {
    Offline = 0,
    Online = 1,
    Disconnected = 2,
    Closing = 3,
};

struct SessionRecord {
    std::string token;       // fence_token
    std::string session_id;
    uint32_t server_id = 0;
    int64_t login_time_sec = 0;
    std::string device_id;
    SessionState state = SessionState::Offline;
    std::string gateway_id;
    int connection_id = 0;
    uint64_t generation = 0;
    int64_t disconnect_deadline_sec = 0;
    std::string gamelogic_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
};

struct AcquireSessionInput {
    uint64_t account_id = 0;
    uint64_t player_id = 0;
    std::string device_id;
    uint32_t server_id = 0;
    uint32_t ttl_sec = 0;
    bool kick_other_device = true;
    std::string gateway_instance_id;
    std::string preferred_gamelogic_instance_id;
};

struct AcquireSessionResult {
    bool ok = false;
    std::string message;
    std::string error_code;
    std::string session_id;
    std::string fence_token;
    uint64_t generation = 0;
    std::string gamelogic_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
    bool kicked_previous = false;
    int64_t login_time_sec = 0;
    uint32_t server_id = 0;
};

class SessionStore {
public:
    static SessionStore &Instance();

    bool InitFromConfig();
    bool Available() const { return available_; }
    int grace_sec() const { return grace_sec_; }

    /** 配置可分配的 GameLogic instance_id 列表（勿写死端口）。 */
    void SetLogicInstanceIds(std::vector<std::string> ids);

    bool AcquireSession(const AcquireSessionInput &in, AcquireSessionResult *out);
    bool Login(const game::LoginReq &req, game::LoginRsp *rsp);
    bool Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp);
    bool Validate(const game::ValidateSessionReq &req, game::ValidateSessionRsp *rsp);
    bool CheckOnline(const game::CheckOnlineReq &req, game::CheckOnlineRsp *rsp);
    bool Logout(const game::LogoutReq &req, game::LogoutRsp *rsp);

    /** 仅 ONLINE 且 token 匹配才通过（DISCONNECTED 拒业务写） */
    bool ValidateToken(uint64_t player_id, const std::string &token, std::string *err);
    bool IsPlayerOnline(uint64_t player_id);

    /** Gateway 在 Login/Reconnect 成功回包后绑定连接 */
    bool BindConnection(uint64_t player_id, const std::string &token, const std::string &gateway_id,
                        int connection_id);
    /**
     * 断线：token+generation 匹配才进入 DISCONNECTED + 宽限期。
     * 旧连接迟到回调因 generation/token 不匹配而忽略。
     * 不等于 Logout。
     */
    bool MarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

private:
    SessionStore() = default;
    std::string SessionKey(uint64_t player_id) const;
    bool LoadSession(uint64_t player_id, SessionRecord *out);
    bool SaveSession(uint64_t player_id, const SessionRecord &rec, int ttl_sec);
    bool ExpireIfGraceElapsed(uint64_t player_id, SessionRecord *rec);
    static std::string StateToString(SessionState s);
    static SessionState StateFromString(const std::string &s);

    bool available_ = false;
    int default_ttl_sec_ = 7200;
    int long_ttl_sec_ = 86400;
    int grace_sec_ = 45;
    std::vector<std::string> logic_instance_ids_{"gl-0"};
};
