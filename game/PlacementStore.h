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

    static std::string StateToString(PlacementState s);
    static PlacementState StateFromString(const std::string &s);

private:
    PlacementStore() = default;
    std::string InstKey(uint64_t id) const;
    std::string TplKey(uint32_t realm, uint64_t tpl) const;
    std::string IdGenKey() const;
    std::string PickOwner(const std::string &preferred) const;

    bool available_ = false;
    int default_lease_sec_ = 30;
    std::string key_prefix_ = "gamemesh:dev:";
    mutable std::mutex cfg_mu_;
    std::vector<std::string> owners_{"gl-0"};
    mutable size_t rr_ = 0;
};
