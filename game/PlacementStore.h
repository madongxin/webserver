#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/** Placement 状态（Redis 字段 state） */
enum class PlacementState {
    Creating = 0,
    Ready = 1,
    Frozen = 2,
    Migrating = 3,
    Recovering = 4,
    Closed = 5,
};

struct PlacementRecord {
    uint32_t realm_id = 0;
    uint64_t map_template_id = 0;
    uint64_t map_instance_id = 0;
    std::string owner_logic_server_id;
    uint64_t owner_epoch = 0;
    uint64_t route_version = 0;
    PlacementState state = PlacementState::Closed;
    int64_t updated_at = 0;
    int64_t lease_until = 0;
};

struct ResolveOrCreateInput {
    uint32_t realm_id = 0;
    uint64_t map_template_id = 0;
    uint64_t map_instance_id = 0;
    std::string preferred_owner;
    bool force_new = false;
};

struct ResolveOrCreateResult {
    bool ok = false;
    std::string message;
    std::string error_code;
    PlacementRecord placement;
};

/**
 * 权威 Map Placement（Session 进程内模块；Redis Lua/CAS）。
 * Gateway/GameLogic 仅缓存，不以本地 MapPlacement 为事实源。
 */
class PlacementStore {
public:
    static PlacementStore &Instance();

    /** 与 SessionStore 共用 RedisPool；在 Session 启动时 Init。 */
    bool InitFromSessionPrefix(const std::string &key_prefix, int default_lease_sec = 30);
    bool Available() const { return available_; }

    void SetLogicOwners(std::vector<std::string> owners);

    bool ResolveOrCreate(const ResolveOrCreateInput &in, ResolveOrCreateResult *out);
    bool Get(uint64_t map_instance_id, PlacementRecord *out);
    bool Migrate(uint64_t map_instance_id, const std::string &new_owner, uint64_t expect_epoch,
                 const std::string &idempotency_key, PlacementRecord *out, std::string *err);
    bool MarkRecovering(uint64_t map_instance_id, const std::string &reason, PlacementRecord *out);
    bool Heartbeat(uint64_t map_instance_id, const std::string &owner, uint64_t epoch,
                   uint32_t lease_sec, int64_t *lease_until_out);
    /** READY 且 lease 已过期 → RECOVERING；lease 仍有效返回 false */
    bool ExpireLeaseToRecovering(uint64_t map_instance_id, PlacementRecord *out, std::string *err);
    int default_lease_sec() const { return default_lease_sec_; }

    /** SCAN 发现 READY+lease 过期 或 RECOVERING 的 map_instance_id（供后台恢复） */
    bool ScanRecoveryCandidates(std::string *cursor, size_t count,
                                std::vector<uint64_t> *expired_ready,
                                std::vector<uint64_t> *recovering);
    /** 选择健康 Owner（可排除旧 Owner） */
    std::string PickHealthyOwner(const std::string &exclude) const;
    /** 审计事件追加到 Redis list（旧/新 Owner、epoch、原因） */
    void AppendAudit(uint64_t map_instance_id, const std::string &event,
                     const std::string &old_owner, const std::string &new_owner, uint64_t old_epoch,
                     uint64_t new_epoch, const std::string &reason);

    static std::string StateToString(PlacementState s);
    static PlacementState StateFromString(const std::string &s);

private:
    PlacementStore() = default;
    std::string InstKey(uint64_t id) const;
    std::string TplKey(uint32_t realm, uint64_t tpl) const;
    std::string IdGenKey() const;
    std::string PickOwner(const std::string &preferred) const;
    /** 当前健康 Owner 列表 CSV，供 ResolveOrCreate Lua 判断软续租 */
    std::string HealthyOwnersCsv() const;

    bool available_ = false;
    int default_lease_sec_ = 30;
    std::string key_prefix_ = "gamemesh:dev:";
    mutable std::mutex cfg_mu_;
    std::vector<std::string> owners_{"gl-0"};
    mutable size_t rr_ = 0;
};
