#include "PlacementStore.h"

#include "Logging.h"
#include "RedisPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace {

int64_t NowUnixSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t ParseU64(const std::string &s) {
    return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

int64_t ParseI64(const std::string &s) {
    return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10));
}

bool FillFromReply(const std::vector<std::string> &r, PlacementRecord *out) {
    // ok, msg, id, realm, tpl, owner, epoch, route_ver, state, updated, lease
    if (!out || r.size() < 11 || r[0] != "1")
        return false;
    out->map_instance_id = ParseU64(r[2]);
    out->realm_id = static_cast<uint32_t>(ParseU64(r[3]));
    out->map_template_id = ParseU64(r[4]);
    out->owner_logic_server_id = r[5];
    out->owner_epoch = ParseU64(r[6]);
    out->route_version = ParseU64(r[7]);
    out->state = PlacementStore::StateFromString(r[8]);
    out->updated_at = ParseI64(r[9]);
    out->lease_until = ParseI64(r[10]);
    return true;
}

// ResolveOrCreate：template 索引原子创建；force_new 跳过索引。
// READY+lease 过期：仅当旧 Owner 仍在健康列表（ARGV[9] CSV）时软续租；否则硬 reclaim。
// 禁止把过期 Placement 当作可进图权威返回（否则 EnterMap → ERR_LEASE_EXPIRED）。
const char kLuaResolveOrCreate[] = R"LUA(
local tpl_key = KEYS[1]
local idgen_key = KEYS[2]
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local owner = ARGV[4]
local now = tonumber(ARGV[5])
local lease = tonumber(ARGV[6])
local force_new = tonumber(ARGV[7])
local want_id = ARGV[8]
local healthy_csv = ARGV[9] or ''

local function load_inst(id)
  local key = prefix .. 'map:inst:' .. id
  local raw = redis.call('HGETALL', key)
  if #raw == 0 then return nil end
  local f = {}
  for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
  return {
    tostring(id), f['realmId'] or realm, f['mapTemplateId'] or tpl,
    f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0',
    f['routeVersion'] or '0', f['state'] or 'CLOSED',
    f['updatedAt'] or '0', f['leaseUntil'] or '0'
  }
end

local function owner_alive(id)
  if id == nil or id == '' or healthy_csv == '' then return false end
  for token in string.gmatch(healthy_csv, '[^,]+') do
    if token == id then return true end
  end
  return false
end

local function usable(L)
  local state = L[7] or ''
  local lease_until = tonumber(L[9]) or 0
  return state == 'READY' and lease_until > now
end

local function reclaim(id, L)
  local key = prefix .. 'map:inst:' .. tostring(id)
  local epoch = tonumber(L[5]) or 0
  local rv = (tonumber(L[6]) or 0) + 1
  local new_epoch = epoch + 1
  local lease_until = now + lease
  local realm_v = L[2] or realm
  local tpl_v = L[3] or tpl
  redis.call('HMSET', key,
    'mapInstanceId', tostring(id),
    'realmId', tostring(realm_v),
    'mapTemplateId', tostring(tpl_v),
    'ownerLogicServerId', owner,
    'ownerEpoch', tostring(new_epoch),
    'routeVersion', tostring(rv),
    'state', 'READY',
    'updatedAt', tostring(now),
    'leaseUntil', tostring(lease_until))
  redis.call('EXPIRE', key, 86400)
  return {'1', 'OK', tostring(id), tostring(realm_v), tostring(tpl_v), owner,
          tostring(new_epoch), tostring(rv), 'READY', tostring(now), tostring(lease_until)}
end

local function soft_renew(L)
  local key = prefix .. 'map:inst:' .. tostring(L[1])
  local lease_until = now + lease
  redis.call('HMSET', key, 'leaseUntil', tostring(lease_until), 'updatedAt', tostring(now),
             'state', 'READY')
  redis.call('EXPIRE', key, 86400)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], 'READY', tostring(now), tostring(lease_until)}
end

local function return_or_reclaim(L)
  if usable(L) then
    return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9]}
  end
  local state = L[7] or ''
  if state == 'CLOSED' then
    return nil
  end
  -- READY+过期：存活 Owner 软续租；死亡 Owner 硬 reclaim（升 epoch/route，换健康 Owner）
  if state == 'READY' then
    if owner_alive(L[4]) then
      return soft_renew(L)
    end
    return reclaim(L[1], L)
  end
  return reclaim(L[1], L)
end

if want_id ~= '0' and want_id ~= '' then
  local L = load_inst(want_id)
  if not L then return {'0', 'NOT_FOUND', 'map instance not found'} end
  if usable(L) then
    return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9]}
  end
  if (L[7] or '') == 'CLOSED' then return {'0', 'CLOSED', 'closed'} end
  if (L[7] or '') == 'READY' then
    if owner_alive(L[4]) then
      return soft_renew(L)
    end
    return reclaim(want_id, L)
  end
  return reclaim(want_id, L)
end

if force_new == 0 and tpl_key ~= '' then
  local existing = redis.call('GET', tpl_key)
  if existing then
    local L = load_inst(existing)
    if L then
      local R = return_or_reclaim(L)
      if R then return R end
      -- CLOSED：删索引后重建
      redis.call('DEL', tpl_key)
    end
  end
end

local id = redis.call('INCR', idgen_key)
local ikey = prefix .. 'map:inst:' .. tostring(id)
local lease_until = now + lease
redis.call('HMSET', ikey,
  'mapInstanceId', tostring(id),
  'realmId', realm,
  'mapTemplateId', tpl,
  'ownerLogicServerId', owner,
  'ownerEpoch', '1',
  'routeVersion', '1',
  'state', 'READY',
  'updatedAt', tostring(now),
  'leaseUntil', tostring(lease_until))
redis.call('EXPIRE', ikey, 86400)
if force_new == 0 and tpl_key ~= '' then
  redis.call('SET', tpl_key, tostring(id), 'NX')
  local canonical = redis.call('GET', tpl_key)
  if canonical and canonical ~= tostring(id) then
    redis.call('DEL', ikey)
    local L = load_inst(canonical)
    if L then
      local R = return_or_reclaim(L)
      if R then return R end
      -- CLOSED race：覆盖索引到本 id 并重建记录
      redis.call('SET', tpl_key, tostring(id))
      redis.call('HMSET', ikey,
        'mapInstanceId', tostring(id),
        'realmId', realm,
        'mapTemplateId', tpl,
        'ownerLogicServerId', owner,
        'ownerEpoch', '1',
        'routeVersion', '1',
        'state', 'READY',
        'updatedAt', tostring(now),
        'leaseUntil', tostring(lease_until))
      redis.call('EXPIRE', ikey, 86400)
    end
  end
end
return {'1', 'OK', tostring(id), realm, tpl, owner, '1', '1', 'READY', tostring(now), tostring(lease_until)}
)LUA";

const char kLuaMigrate[] = R"LUA(
local key = KEYS[1]
local new_owner = ARGV[1]
local expect_epoch = ARGV[2]
local now = tonumber(ARGV[3])
local lease = tonumber(ARGV[4])
local idem = ARGV[5]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if idem ~= '' and (f['lastMigrateIdem'] or '') == idem then
  return {'1', 'OK', f['mapInstanceId'] or '0', f['realmId'] or '0', f['mapTemplateId'] or '0',
          f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0', f['routeVersion'] or '0',
          f['state'] or 'READY', f['updatedAt'] or '0', f['leaseUntil'] or '0'}
end
local state = f['state'] or 'READY'
if state == 'CLOSED' then return {'0', 'CLOSED', 'closed'} end
local lease_until_cur = tonumber(f['leaseUntil'] or '0') or 0
-- 仅当 lease 已过期、RECOVERING/FROZEN，或显式允许时才 Claim 更高 epoch
if state == 'READY' and lease_until_cur > now then
  return {'0', 'LEASE_ACTIVE', 'owner lease still active'}
end
local epoch = tonumber(f['ownerEpoch'] or '0') or 0
if expect_epoch ~= '0' and epoch ~= tonumber(expect_epoch) then
  return {'0', 'EPOCH_MISMATCH', 'expect epoch mismatch'}
end
local new_epoch = epoch + 1
local rv = (tonumber(f['routeVersion'] or '0') or 0) + 1
local lease_until = now + lease
redis.call('HMSET', key,
  'state', 'READY',
  'ownerLogicServerId', new_owner,
  'ownerEpoch', tostring(new_epoch),
  'routeVersion', tostring(rv),
  'updatedAt', tostring(now),
  'leaseUntil', tostring(lease_until),
  'lastMigrateIdem', idem)
redis.call('EXPIRE', key, 86400)
return {'1', 'OK', redis.call('HGET', key, 'mapInstanceId') or string.match(key, '(%d+)$') or '0',
        f['realmId'] or '0', f['mapTemplateId'] or '0', new_owner, tostring(new_epoch),
        tostring(rv), 'READY', tostring(now), tostring(lease_until)}
)LUA";

const char kLuaMarkRecovering[] = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
-- 崩溃恢复：标记 RECOVERING 并立刻让 lease 失效，阻止旧 Owner Heartbeat 复活
redis.call('HMSET', key, 'state', 'RECOVERING', 'updatedAt', tostring(now),
           'leaseUntil', tostring(now))
redis.call('EXPIRE', key, 86400)
return {'1', 'OK', string.match(key, '(%d+)$') or '0', f['realmId'] or '0', f['mapTemplateId'] or '0',
        f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0', f['routeVersion'] or '0',
        'RECOVERING', tostring(now), tostring(now)}
)LUA";

const char kLuaHeartbeat[] = R"LUA(
local key = KEYS[1]
local owner = ARGV[1]
local epoch = ARGV[2]
local now = tonumber(ARGV[3])
local lease = tonumber(ARGV[4])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['ownerLogicServerId'] or '') ~= owner then return {'0', 'OWNER_MISMATCH'} end
if (f['ownerEpoch'] or '') ~= epoch then return {'0', 'EPOCH_MISMATCH'} end
local state = f['state'] or ''
-- RECOVERING 后旧 Owner 不得续租复活
if state ~= 'READY' then return {'0', 'BAD_STATE'} end
local lease_until = now + lease
redis.call('HMSET', key, 'leaseUntil', tostring(lease_until), 'updatedAt', tostring(now))
return {'1', 'OK', tostring(lease_until)}
)LUA";

/** lease 过期且仍为 READY → RECOVERING（Session 扫描） */
const char kLuaExpireToRecovering[] = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local state = f['state'] or ''
if state ~= 'READY' then
  return {'1', 'SKIP', f['state'] or '', f['leaseUntil'] or '0'}
end
local lease_until = tonumber(f['leaseUntil'] or '0') or 0
if lease_until > now then
  return {'0', 'LEASE_ACTIVE', 'still leased'}
end
redis.call('HMSET', key, 'state', 'RECOVERING', 'updatedAt', tostring(now),
           'leaseUntil', tostring(now))
return {'1', 'OK', 'RECOVERING', tostring(now)}
)LUA";

}  // namespace

PlacementStore &PlacementStore::Instance() {
    static PlacementStore g;
    return g;
}

std::string PlacementStore::StateToString(PlacementState s) {
    switch (s) {
    case PlacementState::Creating:
        return "CREATING";
    case PlacementState::Ready:
        return "READY";
    case PlacementState::Frozen:
        return "FROZEN";
    case PlacementState::Migrating:
        return "MIGRATING";
    case PlacementState::Recovering:
        return "RECOVERING";
    default:
        return "CLOSED";
    }
}

PlacementState PlacementStore::StateFromString(const std::string &s) {
    if (s == "CREATING")
        return PlacementState::Creating;
    if (s == "READY")
        return PlacementState::Ready;
    if (s == "FROZEN")
        return PlacementState::Frozen;
    if (s == "MIGRATING")
        return PlacementState::Migrating;
    if (s == "RECOVERING")
        return PlacementState::Recovering;
    return PlacementState::Closed;
}

bool PlacementStore::InitFromSessionPrefix(const std::string &key_prefix, int default_lease_sec) {
    if (!key_prefix.empty())
        key_prefix_ = key_prefix;
    if (default_lease_sec > 0)
        default_lease_sec_ = default_lease_sec;
    available_ = RedisPool::Instance().ready();
    if (available_)
        LOG_INFO << "PlacementStore ready prefix=" << key_prefix_
                 << " lease_sec=" << default_lease_sec_;
    return available_;
}

void PlacementStore::SetLogicOwners(std::vector<std::string> owners) {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (!owners.empty()) {
        owners_ = std::move(owners);
        rr_ = 0;
    }
}

std::string PlacementStore::InstKey(uint64_t id) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%smap:inst:%llu", key_prefix_.c_str(),
                  static_cast<unsigned long long>(id));
    return buf;
}

std::string PlacementStore::TplKey(uint32_t realm, uint64_t tpl) const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%smap:tpl:%u:%llu", key_prefix_.c_str(), realm,
                  static_cast<unsigned long long>(tpl));
    return buf;
}

std::string PlacementStore::IdGenKey() const {
    return key_prefix_ + "map:idgen";
}

std::string PlacementStore::PickOwner(const std::string &preferred) const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (!preferred.empty()) {
        for (const auto &o : owners_) {
            if (o == preferred)
                return preferred;
        }
    }
    if (owners_.empty())
        return "gl-0";
    const size_t idx = rr_ % owners_.size();
    ++rr_;
    return owners_[idx];
}

std::string PlacementStore::HealthyOwnersCsv() const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (owners_.empty())
        return {};
    std::ostringstream os;
    for (size_t i = 0; i < owners_.size(); ++i) {
        if (i)
            os << ',';
        os << owners_[i];
    }
    return os.str();
}

bool PlacementStore::ResolveOrCreate(const ResolveOrCreateInput &in, ResolveOrCreateResult *out) {
    if (!out)
        return false;
    *out = ResolveOrCreateResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.map_template_id == 0 && in.map_instance_id == 0) {
        out->message = "map_template_id or map_instance_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    const std::string owner = PickOwner(in.preferred_owner);
    const std::string healthy_csv = HealthyOwnersCsv();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const std::string tpl_key =
        (in.force_new || in.map_instance_id != 0) ? std::string() : TplKey(in.realm_id, in.map_template_id);
    // KEYS[1] 空字符串时 Lua 仍需要占位；用 dummy 当 force_new
    std::vector<std::string> keys{
        tpl_key.empty() ? (key_prefix_ + "map:tpl:_none") : tpl_key,
        IdGenKey(),
    };
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        owner,
        std::to_string(NowUnixSec()),
        std::to_string(default_lease_sec_),
        in.force_new ? "1" : "0",
        std::to_string(in.map_instance_id),
        healthy_csv,
    };
    // force_new：Lua 里 tpl_key 仍可能 SET NX — 用 force_new=1 跳过索引
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaResolveOrCreate, keys, args, &reply) || reply.size() < 3) {
        out->message = "placement lua failed";
        out->error_code = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : "rejected";
        out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
        return false;
    }
    if (!FillFromReply(reply, &out->placement)) {
        out->message = "bad placement reply";
        out->error_code = "BAD_REPLY";
        return false;
    }
    out->ok = true;
    out->message = "ok";
    return true;
}

bool PlacementStore::Get(uint64_t map_instance_id, PlacementRecord *out) {
    if (!available_ || !out || map_instance_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> fields;
    if (!lease->HGetAll(InstKey(map_instance_id), &fields) || fields.empty())
        return false;
    out->map_instance_id = map_instance_id;
    out->realm_id = static_cast<uint32_t>(ParseU64(fields["realmId"]));
    out->map_template_id = ParseU64(fields["mapTemplateId"]);
    out->owner_logic_server_id = fields["ownerLogicServerId"];
    out->owner_epoch = ParseU64(fields["ownerEpoch"]);
    out->route_version = ParseU64(fields["routeVersion"]);
    out->state = StateFromString(fields["state"]);
    out->updated_at = ParseI64(fields["updatedAt"]);
    out->lease_until = ParseI64(fields["leaseUntil"]);
    return true;
}

bool PlacementStore::Migrate(uint64_t map_instance_id, const std::string &new_owner,
                             uint64_t expect_epoch, const std::string &idempotency_key,
                             PlacementRecord *out, std::string *err) {
    if (!available_ || map_instance_id == 0 || new_owner.empty()) {
        if (err)
            *err = "invalid arg";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        if (err)
            *err = "pool exhausted";
        return false;
    }
    std::vector<std::string> keys{InstKey(map_instance_id)};
    std::vector<std::string> args{new_owner, std::to_string(expect_epoch),
                                  std::to_string(NowUnixSec()), std::to_string(default_lease_sec_),
                                  idempotency_key};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaMigrate, keys, args, &reply) || reply.size() < 3) {
        if (err)
            *err = "lua failed";
        return false;
    }
    if (reply[0] != "1") {
        if (err)
            *err = reply.size() > 2 ? reply[2] : reply[1];
        return false;
    }
    PlacementRecord rec;
    if (!FillFromReply(reply, &rec)) {
        if (err)
            *err = "bad reply";
        return false;
    }
    rec.map_instance_id = map_instance_id;
    if (out)
        *out = rec;
    LOG_INFO << "PlacementStore Migrate map=" << map_instance_id << " -> " << new_owner
             << " epoch=" << rec.owner_epoch;
    return true;
}

bool PlacementStore::MarkRecovering(uint64_t map_instance_id, const std::string &reason,
                                    PlacementRecord *out) {
    (void)reason;
    if (!available_ || map_instance_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{InstKey(map_instance_id)};
    std::vector<std::string> args{std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaMarkRecovering, keys, args, &reply) || reply.size() < 3)
        return false;
    if (reply[0] != "1")
        return false;
    PlacementRecord rec;
    if (!FillFromReply(reply, &rec))
        return false;
    rec.map_instance_id = map_instance_id;
    if (out)
        *out = rec;
    LOG_INFO << "PlacementStore RECOVERING map=" << map_instance_id;
    return true;
}

bool PlacementStore::Heartbeat(uint64_t map_instance_id, const std::string &owner, uint64_t epoch,
                               uint32_t lease_sec, int64_t *lease_until_out) {
    if (!available_ || map_instance_id == 0 || owner.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    const int ls = lease_sec > 0 ? static_cast<int>(lease_sec) : default_lease_sec_;
    std::vector<std::string> keys{InstKey(map_instance_id)};
    std::vector<std::string> args{owner, std::to_string(epoch), std::to_string(NowUnixSec()),
                                  std::to_string(ls)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaHeartbeat, keys, args, &reply) || reply.size() < 2)
        return false;
    if (reply[0] != "1")
        return false;
    if (lease_until_out && reply.size() > 2)
        *lease_until_out = ParseI64(reply[2]);
    return true;
}

bool PlacementStore::ExpireLeaseToRecovering(uint64_t map_instance_id, PlacementRecord *out,
                                             std::string *err) {
    if (!available_ || map_instance_id == 0) {
        if (err)
            *err = "invalid";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        if (err)
            *err = "pool exhausted";
        return false;
    }
    std::vector<std::string> keys{InstKey(map_instance_id)};
    std::vector<std::string> args{std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaExpireToRecovering, keys, args, &reply) || reply.size() < 2) {
        if (err)
            *err = "lua failed";
        return false;
    }
    if (reply[0] != "1" || (reply.size() > 1 && reply[1] != "OK")) {
        if (err)
            *err = reply.size() > 2 ? reply[2] : (reply.size() > 1 ? reply[1] : "rejected");
        return false;
    }
    if (out)
        Get(map_instance_id, out);
    LOG_INFO << "PlacementStore lease expired -> RECOVERING map=" << map_instance_id;
    return true;
}

bool PlacementStore::ScanRecoveryCandidates(std::string *cursor, size_t count,
                                            std::vector<uint64_t> *expired_ready,
                                            std::vector<uint64_t> *recovering) {
    if (!available_ || !cursor || !expired_ready || !recovering)
        return false;
    expired_ready->clear();
    recovering->clear();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    const std::string match = key_prefix_ + "map:inst:*";
    const char *scan_lua = R"LUA(
local cursor = ARGV[1]
local match = ARGV[2]
local cnt = tonumber(ARGV[3]) or 32
local now = tonumber(ARGV[4])
local r = redis.call('SCAN', cursor, 'MATCH', match, 'COUNT', cnt)
local nextc = r[1]
local keys = r[2]
local out = {nextc}
for _, k in ipairs(keys) do
  local state = redis.call('HGET', k, 'state') or ''
  local lease_until = tonumber(redis.call('HGET', k, 'leaseUntil') or '0') or 0
  local mid = redis.call('HGET', k, 'mapInstanceId') or string.match(k, '(%d+)$') or '0'
  if state == 'READY' and lease_until > 0 and lease_until <= now then
    out[#out + 1] = 'E:' .. mid
  elseif state == 'RECOVERING' then
    out[#out + 1] = 'R:' .. mid
  end
end
return out
)LUA";
    std::vector<std::string> keys;
    std::vector<std::string> args{*cursor, match, std::to_string(count > 0 ? count : 32),
                                  std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(scan_lua, keys, args, &reply) || reply.empty())
        return false;
    *cursor = reply[0];
    for (size_t i = 1; i < reply.size(); ++i) {
        const std::string &t = reply[i];
        if (t.size() < 3)
            continue;
        const uint64_t id = ParseU64(t.substr(2));
        if (id == 0)
            continue;
        if (t[0] == 'E')
            expired_ready->push_back(id);
        else if (t[0] == 'R')
            recovering->push_back(id);
    }
    return true;
}

std::string PlacementStore::PickHealthyOwner(const std::string &exclude) const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    if (owners_.empty())
        return "gl-0";
    for (size_t i = 0; i < owners_.size(); ++i) {
        const size_t idx = (rr_ + i) % owners_.size();
        if (owners_[idx] != exclude)
            return owners_[idx];
    }
    return owners_[rr_ % owners_.size()];
}

void PlacementStore::AppendAudit(uint64_t map_instance_id, const std::string &event,
                                 const std::string &old_owner, const std::string &new_owner,
                                 uint64_t old_epoch, uint64_t new_epoch,
                                 const std::string &reason) {
    if (!available_)
        return;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return;
    std::ostringstream oss;
    oss << NowUnixSec() << '|' << map_instance_id << '|' << event << '|' << old_owner << "->"
        << new_owner << "|epoch=" << old_epoch << "->" << new_epoch << '|' << reason;
    const std::string key = key_prefix_ + "map:audit";
    const char *lua = R"LUA(
redis.call('LPUSH', KEYS[1], ARGV[1])
redis.call('LTRIM', KEYS[1], 0, 999)
redis.call('EXPIRE', KEYS[1], 604800)
return {'1'}
)LUA";
    std::vector<std::string> keys{key};
    std::vector<std::string> args{oss.str()};
    std::vector<std::string> reply;
    lease->Eval(lua, keys, args, &reply);
    LOG_INFO << "PlacementAudit " << oss.str();
}
