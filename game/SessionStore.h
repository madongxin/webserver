#pragma once

#include "game.pb.h"

#include <cstdint>
#include <mutex>
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
    std::string token;  // fence_token
    std::string session_id;
    uint32_t server_id = 0;
    int64_t login_time_sec = 0;
    std::string device_id;
    SessionState state = SessionState::Offline;
    std::string gateway_id;
    uint64_t connection_id = 0;
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
    /** 非空时 Redis 幂等：同 operation_id 重试返回同一 Session/fence */
    std::string operation_id;
};

enum class SessionOpStatus {
    NotFound = 0,
    Pending = 1,
    Done = 2,
};

struct ReconnectSessionInput {
    uint64_t player_id = 0;
    std::string session_id;
    std::string reconnect_ticket;
    std::string gateway_instance_id;
    uint64_t last_server_seq = 0;
    std::string operation_id;
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
    const std::string &key_prefix() const { return key_prefix_; }

    /** 配置可分配的 GameLogic instance_id 列表（勿写死端口）。 */
    void SetLogicInstanceIds(std::vector<std::string> ids);
    std::vector<std::string> LogicInstanceIds() const;

    bool AcquireSession(const AcquireSessionInput &in, AcquireSessionResult *out);
    bool Login(const game::LoginReq &req, game::LoginRsp *rsp);
    bool Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp);
    /** Reconnect 并输出权威路由（logic/map/epoch/version） */
    bool Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp, SessionRecord *route_out);
    /** 带 operation_id 幂等的重连（Gateway ReconnectV2） */
    bool ReconnectSession(const ReconnectSessionInput &in, AcquireSessionResult *out,
                          SessionRecord *route_out);
    /** 查询幂等操作结果（超时后结果未知） */
    bool GetSessionOperation(const std::string &operation_id, SessionOpStatus *status,
                             std::string *op_kind, AcquireSessionResult *out);
    bool Validate(const game::ValidateSessionReq &req, game::ValidateSessionRsp *rsp);
    bool CheckOnline(const game::CheckOnlineReq &req, game::CheckOnlineRsp *rsp);
    bool Logout(const game::LogoutReq &req, game::LogoutRsp *rsp);

    /** 仅 ONLINE 且 token 匹配才通过（DISCONNECTED 拒业务写） */
    bool ValidateToken(uint64_t player_id, const std::string &token, std::string *err);
    bool IsPlayerOnline(uint64_t player_id);

    /** Gateway 在 Login/Reconnect 成功回包后绑定连接 */
    bool BindConnection(uint64_t player_id, const std::string &token, const std::string &gateway_id,
                        uint64_t connection_id);
    /**
     * 断线：token+generation 匹配才进入 DISCONNECTED + 宽限期。
     * 旧连接迟到回调因 generation/token 不匹配而忽略。
     * 不等于 Logout。
     */
    bool MarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

    /** 进图/迁移后更新权威路由（fence CAS；route_version 单调） */
    bool UpdatePlayerRoute(uint64_t player_id, const std::string &fence_token,
                           const std::string &gamelogic_instance_id, uint64_t map_instance_id,
                           uint64_t map_owner_epoch, uint64_t route_version,
                           const std::string &gateway_instance_id, const std::string &push_endpoint,
                           uint64_t *route_version_out, std::string *err);

    struct TransferBeginIn {
        uint64_t player_id = 0;
        std::string fence_token;
        uint64_t expected_route_version = 0;
        std::string from_logic;
        std::string to_logic;
        uint64_t map_instance_id = 0;
        uint64_t map_owner_epoch = 0;
        std::string transfer_id;
        std::string gateway_instance_id;
    };
    struct TransferBeginOut {
        bool ok = false;
        std::string message;
        std::string error_code;
        std::string transfer_id;
        uint64_t route_version = 0;
        std::string route_state;
    };
    bool BeginPlayerTransfer(const TransferBeginIn &in, TransferBeginOut *out);

    struct TransferCommitIn {
        uint64_t player_id = 0;
        std::string fence_token;
        std::string transfer_id;
        std::string to_logic;
        uint64_t map_instance_id = 0;
        uint64_t map_owner_epoch = 0;
        std::string gateway_instance_id;
    };
    struct TransferCommitOut {
        bool ok = false;
        std::string message;
        std::string error_code;
        uint64_t route_version = 0;
        std::string gamelogic_instance_id;
        uint64_t map_instance_id = 0;
        uint64_t map_owner_epoch = 0;
        std::string route_state;
    };
    bool CommitPlayerTransfer(const TransferCommitIn &in, TransferCommitOut *out);
    bool AbortPlayerTransfer(uint64_t player_id, const std::string &fence_token,
                             const std::string &transfer_id, std::string *err,
                             uint64_t *route_version_out);
    bool GetPlayerRoute(uint64_t player_id, const std::string &fence_token, SessionRecord *out,
                        std::string *route_state, std::string *transfer_id, std::string *err);

private:
    SessionStore() = default;
    std::string SessionKey(uint64_t player_id) const;
    std::string OpKey(const std::string &operation_id) const;
    bool LoadSession(uint64_t player_id, SessionRecord *out);
    bool ExpireIfGraceElapsed(uint64_t player_id, SessionRecord *rec);
    static std::string StateToString(SessionState s);
    static SessionState StateFromString(const std::string &s);

    enum class OpBegin { Execute, Done, Pending, Error };
    OpBegin BeginOperation(const std::string &operation_id, AcquireSessionResult *cached,
                           std::string *op_kind_out, std::string *err);
    bool CompleteOperation(const std::string &operation_id, const std::string &op_kind,
                           const AcquireSessionResult &result);
    bool AbortOperation(const std::string &operation_id);
    bool LoadOperationResult(const std::string &operation_id, SessionOpStatus *status,
                             std::string *op_kind, AcquireSessionResult *out);
    bool AcquireSessionUnlocked(const AcquireSessionInput &in, AcquireSessionResult *out);

    bool available_ = false;
    int default_ttl_sec_ = 7200;
    int long_ttl_sec_ = 86400;
    int grace_sec_ = 45;
    int pool_size_ = 8;
    std::string key_prefix_ = "gamemesh:dev:";
    std::vector<std::string> logic_instance_ids_{"gl-0"};
    /** 保护 logic_instance_ids_ 等进程内配置；Redis 权威状态靠 Lua */
    mutable std::mutex cfg_mu_;
};
