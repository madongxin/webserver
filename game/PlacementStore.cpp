#include "PlacementStore.h"

#include "HealthyLogicSnapshot.h"
#include "Logging.h"
#include "RedisPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

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
    if (r.size() > 13)
        out->kind = r[13];
    if (r.size() > 14)
        out->line_no = static_cast<uint32_t>(ParseU64(r[14]));
    if (r.size() > 15)
        out->soft_cap = static_cast<uint32_t>(ParseU64(r[15]));
    if (r.size() > 16)
        out->hard_cap = static_cast<uint32_t>(ParseU64(r[16]));
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

-- 显式 preferred（ARGV owner）仍健康则用；否则 idgen % |healthy|
local function pick_new_owner(new_id)
  local healthy = {}
  for token in string.gmatch(healthy_csv, '[^,]+') do
    healthy[#healthy + 1] = token
  end
  if owner ~= '' then
    for i = 1, #healthy do
      if healthy[i] == owner then
        return owner
      end
    end
  end
  if #healthy == 0 then
    return owner
  end
  local n = tonumber(new_id) or 0
  return healthy[(n % #healthy) + 1]
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
  local chosen = pick_new_owner(id)
  redis.call('HMSET', key,
    'mapInstanceId', tostring(id),
    'realmId', tostring(realm_v),
    'mapTemplateId', tostring(tpl_v),
    'ownerLogicServerId', chosen,
    'ownerEpoch', tostring(new_epoch),
    'routeVersion', tostring(rv),
    'state', 'READY',
    'updatedAt', tostring(now),
    'leaseUntil', tostring(lease_until))
  redis.call('EXPIRE', key, 86400)
  return {'1', 'OK', tostring(id), tostring(realm_v), tostring(tpl_v), chosen,
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
    else
      -- 孤儿索引（inst 已过期/删除）：清掉再创建，避免后续返回幽灵 id
      redis.call('DEL', tpl_key)
    end
  end
end

local id = redis.call('INCR', idgen_key)
local chosen = pick_new_owner(id)
local ikey = prefix .. 'map:inst:' .. tostring(id)
local lease_until = now + lease
local function write_new_inst()
  redis.call('HMSET', ikey,
    'mapInstanceId', tostring(id),
    'realmId', realm,
    'mapTemplateId', tpl,
    'ownerLogicServerId', chosen,
    'ownerEpoch', '1',
    'routeVersion', '1',
    'state', 'READY',
    'updatedAt', tostring(now),
    'leaseUntil', tostring(lease_until))
  redis.call('EXPIRE', ikey, 86400)
end
write_new_inst()
if force_new == 0 and tpl_key ~= '' then
  redis.call('SET', tpl_key, tostring(id), 'NX')
  redis.call('EXPIRE', tpl_key, 86400)
  local canonical = redis.call('GET', tpl_key)
  if canonical and canonical ~= tostring(id) then
    redis.call('DEL', ikey)
    local L = load_inst(canonical)
    if L then
      local R = return_or_reclaim(L)
      if R then return R end
      -- CLOSED race：覆盖索引到本 id 并重建记录
      redis.call('SET', tpl_key, tostring(id))
      redis.call('EXPIRE', tpl_key, 86400)
      write_new_inst()
    else
      -- 并发窗口内 canonical 变孤儿：认领本 id，禁止返回已 DEL 的幽灵记录
      redis.call('SET', tpl_key, tostring(id))
      redis.call('EXPIRE', tpl_key, 86400)
      write_new_inst()
    end
  end
end
return {'1', 'OK', tostring(id), realm, tpl, chosen, '1', '1', 'READY', tostring(now), tostring(lease_until)}
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
-- READY 且 lease 仍有效禁止热迁；DRAINING/RECOVERING/FROZEN 允许换 Owner
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

// 公共池原子占位：选择 READY+lease+count<cap 或新建；指定实例满员不换图。
const char kLuaReserveSlot[] = R"LUA(
local pool_key = KEYS[1]
local idgen_key = KEYS[2]
local pres_key = KEYS[3]
local op_key = KEYS[4]
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local player = ARGV[4]
local op = ARGV[5]
local capacity = tonumber(ARGV[6]) or 50
local want_id = ARGV[7]
local owner = ARGV[8]
local now = tonumber(ARGV[9])
local lease = tonumber(ARGV[10])
local healthy_csv = ARGV[11] or ''

local function pick_new_owner(new_id)
  local healthy = {}
  for token in string.gmatch(healthy_csv, '[^,]+') do
    healthy[#healthy + 1] = token
  end
  if owner ~= '' then
    for i = 1, #healthy do
      if healthy[i] == owner then
        return owner
      end
    end
  end
  if #healthy == 0 then
    return owner
  end
  local n = tonumber(new_id) or 0
  return healthy[(n % #healthy) + 1]
end

local function load_inst(id)
  local key = prefix .. 'map:inst:' .. tostring(id)
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

local function usable(L)
  return (L[7] or '') == 'READY' and (tonumber(L[9]) or 0) > now
end

local function occ_key(id)
  return prefix .. 'map:occ:' .. tostring(id)
end

local function pack(L, occ, idem)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9],
          tostring(occ or 0), idem or '0'}
end

local function save_pres(id)
  redis.call('HMSET', pres_key,
    'mapInstanceId', tostring(id),
    'realmId', realm,
    'mapTemplateId', tpl,
    'state', 'reserved',
    'operationId', op)
  redis.call('EXPIRE', pres_key, 86400)
end

local function cache_op(reply)
  if op == nil or op == '' then return end
  local enc = table.concat(reply, '\t')
  redis.call('SET', op_key, enc, 'EX', 600)
end

if op ~= '' then
  local cached = redis.call('GET', op_key)
  if cached then
    local R = {}
    for token in string.gmatch(cached, '[^\t]+') do
      R[#R + 1] = token
    end
    if #R >= 3 then
      R[13] = '1'
      return R
    end
  end
end

local function try_join(id)
  local L = load_inst(id)
  if not L or not usable(L) then return nil, 'NOT_READY' end
  local ok = occ_key(id)
  if redis.call('SISMEMBER', ok, player) == 1 then
    return L, tonumber(redis.call('SCARD', ok))
  end
  local n = tonumber(redis.call('SCARD', ok)) or 0
  if n >= capacity then return nil, 'FULL' end
  redis.call('SADD', ok, player)
  redis.call('EXPIRE', ok, 86400)
  redis.call('ZREM', prefix .. 'map:idle', tostring(id))
  return L, n + 1
end

local existing = redis.call('HGET', pres_key, 'mapInstanceId')
if existing and existing ~= '' then
  local L, n = try_join(existing)
  if L then
    save_pres(existing)
    local R = pack(L, n, '1')
    cache_op(R)
    return R
  end
  redis.call('DEL', pres_key)
end

if want_id ~= '0' and want_id ~= '' then
  local L, n = try_join(want_id)
  if not L then
    if n == 'FULL' then return {'0', 'ERR_MAP_FULL', 'map instance full'} end
    return {'0', 'NOT_READY', 'map instance not joinable'}
  end
  save_pres(want_id)
  local R = pack(L, n, '0')
  cache_op(R)
  return R
end

local function pool_type(k)
  local t = redis.call('TYPE', k)
  if type(t) == 'table' then t = t['ok'] or '' end
  return t
end

-- 旧 SET 无序；迁到 ZSET，score=instance id（INCR 即创建序）
local function pool_ids()
  local t = pool_type(pool_key)
  if t == 'set' then
    local old = redis.call('SMEMBERS', pool_key)
    redis.call('DEL', pool_key)
    for _, id in ipairs(old) do
      redis.call('ZADD', pool_key, tonumber(id) or 0, tostring(id))
    end
  end
  return redis.call('ZRANGE', pool_key, 0, -1)
end

local members = pool_ids()
for _, id in ipairs(members) do
  local L, n = try_join(id)
  if L then
    save_pres(id)
    local R = pack(L, n, '0')
    cache_op(R)
    return R
  end
end

local id = redis.call('INCR', idgen_key)
local chosen = pick_new_owner(id)
local ikey = prefix .. 'map:inst:' .. tostring(id)
local lease_until = now + lease
redis.call('HMSET', ikey,
  'mapInstanceId', tostring(id),
  'realmId', realm,
  'mapTemplateId', tpl,
  'ownerLogicServerId', chosen,
  'ownerEpoch', '1',
  'routeVersion', '1',
  'state', 'READY',
  'updatedAt', tostring(now),
  'leaseUntil', tostring(lease_until))
redis.call('EXPIRE', ikey, 86400)
redis.call('ZADD', pool_key, id, tostring(id))
redis.call('EXPIRE', pool_key, 86400)
redis.call('SADD', occ_key(id), player)
redis.call('EXPIRE', occ_key(id), 86400)
save_pres(id)
local L = {tostring(id), realm, tpl, chosen, '1', '1', 'READY', tostring(now), tostring(lease_until)}
local R = pack(L, 1, '0')
cache_op(R)
return R
)LUA";

const char kLuaReserveLine[] = R"LUA(
local lines_key = KEYS[1]
local idgen_key = KEYS[2]
local pres_key = KEYS[3]
local op_key = KEYS[4]
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local player = ARGV[4]
local op = ARGV[5]
local soft = tonumber(ARGV[6]) or 200
local hard = tonumber(ARGV[7]) or 400
local max_lines = tonumber(ARGV[8]) or 8
local want_id = ARGV[9]
local want_line = tonumber(ARGV[10]) or 0
local owner = ARGV[11]
local now = tonumber(ARGV[12])
local lease = tonumber(ARGV[13])
local healthy_csv = ARGV[14] or ''
local empty_delay = tonumber(ARGV[15]) or 300
local min_lines = tonumber(ARGV[16]) or 1
local qtok = ARGV[17] or ''
if soft < 1 then soft = 1 end
if hard < soft then hard = soft end
if max_lines < 1 then max_lines = 1 end

local function pick_new_owner(new_id)
  local healthy = {}
  for token in string.gmatch(healthy_csv, '[^,]+') do
    healthy[#healthy + 1] = token
  end
  if owner ~= '' then
    for i = 1, #healthy do
      if healthy[i] == owner then
        return owner
      end
    end
  end
  if #healthy == 0 then
    return owner
  end
  local n = tonumber(new_id) or 0
  return healthy[(n % #healthy) + 1]
end

local function load_inst(id)
  local key = prefix .. 'map:inst:' .. tostring(id)
  local raw = redis.call('HGETALL', key)
  if #raw == 0 then return nil end
  local f = {}
  for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
  return {
    tostring(id), f['realmId'] or realm, f['mapTemplateId'] or tpl,
    f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0',
    f['routeVersion'] or '0', f['state'] or 'CLOSED',
    f['updatedAt'] or '0', f['leaseUntil'] or '0',
    f['kind'] or 'LINE', tonumber(f['lineNo'] or '0') or 0,
    tonumber(f['softCap'] or tostring(soft)) or soft,
    tonumber(f['hardCap'] or tostring(hard)) or hard
  }
end

local function usable(L)
  return (L[7] or '') == 'READY' and (tonumber(L[9]) or 0) > now
end

local function occ_key(id)
  return prefix .. 'map:occ:' .. tostring(id)
end

local function line_key(n)
  return prefix .. 'map:line:' .. realm .. ':' .. tpl .. ':' .. tostring(n)
end

local function pack(L, occ, idem)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9],
          tostring(occ or 0), idem or '0', L[10] or 'LINE',
          tostring(L[11] or 0), tostring(L[12] or soft), tostring(L[13] or hard)}
end

local function save_pres(id)
  redis.call('HMSET', pres_key,
    'mapInstanceId', tostring(id),
    'realmId', realm,
    'mapTemplateId', tpl,
    'state', 'reserved',
    'operationId', op,
    'kind', 'LINE')
  redis.call('EXPIRE', pres_key, 86400)
end

local function cache_op(reply)
  if op == nil or op == '' then return end
  local enc = table.concat(reply, '\t')
  redis.call('SET', op_key, enc, 'EX', 600)
end

if op ~= '' then
  local cached = redis.call('GET', op_key)
  if cached then
    local R = {}
    for token in string.gmatch(cached, '[^\t]+') do
      R[#R + 1] = token
    end
    if #R >= 3 then
      R[13] = '1'
      return R
    end
  end
end

local function try_join(id, cap)
  local L = load_inst(id)
  if not L or not usable(L) then return nil, 'NOT_READY' end
  local ok = occ_key(id)
  if redis.call('SISMEMBER', ok, player) == 1 then
    return L, tonumber(redis.call('SCARD', ok))
  end
  local n = tonumber(redis.call('SCARD', ok)) or 0
  if n >= cap then return nil, 'FULL' end
  redis.call('SADD', ok, player)
  redis.call('EXPIRE', ok, 86400)
  redis.call('ZREM', prefix .. 'map:idle', tostring(id))
  return L, n + 1
end

if qtok ~= '' then
  local tkey = prefix .. 'map:qtok:' .. qtok
  local traw = redis.call('HGETALL', tkey)
  if #traw == 0 then return {'0', 'ERR_QUEUE_INVALID', 'queue token invalid'} end
  local tf = {}
  for i = 1, #traw, 2 do tf[traw[i]] = traw[i + 1] end
  if (tf['player'] or '') ~= player then
    return {'0', 'ERR_QUEUE_INVALID', 'queue token player mismatch'}
  end
  local qline = tonumber(tf['lineNo'] or '0') or 0
  if qline < 1 then return {'0', 'ERR_QUEUE_INVALID', 'queue token line'} end
  local qkey = prefix .. 'map:queue:' .. realm .. ':' .. tpl .. ':' .. tostring(qline)
  local rank = redis.call('ZRANK', qkey, player)
  if rank == false then return {'0', 'ERR_QUEUE_INVALID', 'not in queue'} end
  if tonumber(rank) ~= 0 then
    return {'0', 'ERR_QUEUE_NOT_READY', 'queue not head'}
  end
  local qid = redis.call('GET', line_key(qline))
  if not qid or qid == '' then return {'0', 'ERR_MAP_NO_LINE', 'queue line gone'} end
  local L, n = try_join(qid, hard)
  if not L then
    if n == 'FULL' then return {'0', 'ERR_MAP_LINE_FULL', 'map line full'} end
    return {'0', 'ERR_MAP_NOT_READY', 'map line not joinable'}
  end
  redis.call('ZREM', qkey, player)
  redis.call('DEL', tkey)
  redis.call('DEL', prefix .. 'map:qplayer:' .. player)
  save_pres(qid)
  local R = pack(L, n, '0')
  cache_op(R)
  return R
end

local existing = redis.call('HGET', pres_key, 'mapInstanceId')
if existing and existing ~= '' then
  local L, n = try_join(existing, hard)
  if L then
    save_pres(existing)
    local R = pack(L, n, '1')
    cache_op(R)
    return R
  end
  redis.call('DEL', pres_key)
end

if want_id ~= '0' and want_id ~= '' then
  local L, n = try_join(want_id, hard)
  if not L then
    if n == 'FULL' then return {'0', 'ERR_MAP_FULL', 'map instance full'} end
    return {'0', 'NOT_READY', 'map instance not joinable'}
  end
  save_pres(want_id)
  local R = pack(L, n, '0')
  cache_op(R)
  return R
end

if want_line > 0 then
  local id = redis.call('GET', line_key(want_line))
  if not id or id == '' then
    return {'0', 'ERR_MAP_NO_LINE', 'map line not found'}
  end
  local L, n = try_join(id, hard)
  if not L then
    if n == 'FULL' then return {'0', 'ERR_MAP_LINE_FULL', 'map line full'} end
    return {'0', 'NOT_READY', 'map line not joinable'}
  end
  save_pres(id)
  local R = pack(L, n, '0')
  cache_op(R)
  return R
end

local function line_ids()
  return redis.call('ZRANGE', lines_key, 0, -1)
end

local members = line_ids()
local best_id, best_n, best_line = nil, nil, nil
local under_hard_id, under_hard_n, under_hard_line = nil, nil, nil
for _, id in ipairs(members) do
  local L = load_inst(id)
  if L and usable(L) then
    local n = tonumber(redis.call('SCARD', occ_key(id))) or 0
    local ln = tonumber(L[11]) or 0
    if n < soft then
      if best_id == nil or n < best_n or (n == best_n and ln < best_line) then
        best_id, best_n, best_line = id, n, ln
      end
    end
    if n < hard then
      if under_hard_id == nil or n < under_hard_n or (n == under_hard_n and ln < under_hard_line) then
        under_hard_id, under_hard_n, under_hard_line = id, n, ln
      end
    end
  end
end

if best_id then
  local L, n = try_join(best_id, soft)
  if L then
    save_pres(best_id)
    local R = pack(L, n, '0')
    cache_op(R)
    return R
  end
end

local live = 0
for _, id in ipairs(members) do
  local L = load_inst(id)
  if L and usable(L) then live = live + 1 end
end

if live >= max_lines then
  if under_hard_id then
    local L, n = try_join(under_hard_id, hard)
    if L then
      save_pres(under_hard_id)
      local R = pack(L, n, '0')
      cache_op(R)
      return R
    end
  end
  return {'0', 'ERR_MAP_LINE_LIMIT', 'map line limit'}
end

local maxn = 0
for _, id in ipairs(members) do
  local L = load_inst(id)
  if L then
    local ln = tonumber(L[11]) or 0
    if ln > maxn then maxn = ln end
  end
end
local new_line = maxn + 1
local id = redis.call('INCR', idgen_key)
local chosen = pick_new_owner(id)
local lkey = line_key(new_line)
local nx = redis.call('SET', lkey, tostring(id), 'NX')
if not nx then
  local win = redis.call('GET', lkey)
  if win and win ~= '' then
    local L, n = try_join(win, soft)
    if not L then L, n = try_join(win, hard) end
    if L then
      save_pres(win)
      local R = pack(L, n, '0')
      cache_op(R)
      return R
    end
  end
  return {'0', 'ERR_MAP_LINE_LIMIT', 'map line create race'}
end
redis.call('EXPIRE', lkey, 86400)
local ikey = prefix .. 'map:inst:' .. tostring(id)
local lease_until = now + lease
redis.call('HMSET', ikey,
  'mapInstanceId', tostring(id),
  'realmId', realm,
  'mapTemplateId', tpl,
  'ownerLogicServerId', chosen,
  'ownerEpoch', '1',
  'routeVersion', '1',
  'state', 'READY',
  'updatedAt', tostring(now),
  'leaseUntil', tostring(lease_until),
  'kind', 'LINE',
  'lineNo', tostring(new_line),
  'softCap', tostring(soft),
  'hardCap', tostring(hard),
  'emptyCloseDelay', tostring(empty_delay),
  'minLines', tostring(min_lines))
redis.call('EXPIRE', ikey, 86400)
redis.call('ZADD', lines_key, new_line, tostring(id))
redis.call('EXPIRE', lines_key, 86400)
redis.call('SADD', occ_key(id), player)
redis.call('EXPIRE', occ_key(id), 86400)
save_pres(id)
local L = {tostring(id), realm, tpl, chosen, '1', '1', 'READY', tostring(now),
           tostring(lease_until), 'LINE', new_line, soft, hard}
local R = pack(L, 1, '0')
cache_op(R)
return R
)LUA";

const char kLuaSwitchLine[] = R"LUA(
local pres_key = KEYS[1]
local op_key = KEYS[2]
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local player = ARGV[4]
local op = ARGV[5]
local want_line = tonumber(ARGV[6]) or 0
local soft = tonumber(ARGV[7]) or 200
local hard = tonumber(ARGV[8]) or 400
local now = tonumber(ARGV[9])
if soft < 1 then soft = 1 end
if hard < soft then hard = soft end
if want_line < 1 then return {'0', 'ERR_INVALID_ARGUMENT', 'line_no required'} end

local function load_inst(id)
  local key = prefix .. 'map:inst:' .. tostring(id)
  local raw = redis.call('HGETALL', key)
  if #raw == 0 then return nil end
  local f = {}
  for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
  return {
    tostring(id), f['realmId'] or realm, f['mapTemplateId'] or tpl,
    f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0',
    f['routeVersion'] or '0', f['state'] or 'CLOSED',
    f['updatedAt'] or '0', f['leaseUntil'] or '0',
    f['kind'] or 'LINE', tonumber(f['lineNo'] or '0') or 0,
    tonumber(f['softCap'] or tostring(soft)) or soft,
    tonumber(f['hardCap'] or tostring(hard)) or hard
  }
end

local function usable(L)
  return (L[7] or '') == 'READY' and (tonumber(L[9]) or 0) > now
end

local function occ_key(id)
  return prefix .. 'map:occ:' .. tostring(id)
end

local function pack(L, occ, idem)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9],
          tostring(occ or 0), idem or '0', L[10] or 'LINE',
          tostring(L[11] or 0), tostring(L[12] or soft), tostring(L[13] or hard)}
end

local function save_pres(id)
  redis.call('HMSET', pres_key,
    'mapInstanceId', tostring(id),
    'realmId', realm,
    'mapTemplateId', tpl,
    'state', 'reserved',
    'operationId', op,
    'kind', 'LINE')
  redis.call('EXPIRE', pres_key, 86400)
end

local function cache_op(reply)
  if op == nil or op == '' then return end
  local enc = table.concat(reply, '\t')
  redis.call('SET', op_key, enc, 'EX', 600)
end

if op ~= '' then
  local cached = redis.call('GET', op_key)
  if cached then
    local R = {}
    for token in string.gmatch(cached, '[^\t]+') do
      R[#R + 1] = token
    end
    if #R >= 3 then
      R[13] = '1'
      return R
    end
  end
end

local old_id = redis.call('HGET', pres_key, 'mapInstanceId')
if not old_id or old_id == '' then
  return {'0', 'ERR_NOT_ON_MAP', 'not on a line'}
end
local old = load_inst(old_id)
if not old or (old[10] or '') ~= 'LINE' then
  return {'0', 'ERR_NOT_ON_MAP', 'not on a line instance'}
end
if (old[3] or '') ~= tpl then
  return {'0', 'ERR_INVALID_ARGUMENT', 'template mismatch'}
end
if tonumber(old[11]) == want_line then
  local n = tonumber(redis.call('SCARD', occ_key(old_id))) or 0
  local R = pack(old, n, '1')
  cache_op(R)
  return R
end

local lkey = prefix .. 'map:line:' .. realm .. ':' .. tpl .. ':' .. tostring(want_line)
local target = redis.call('GET', lkey)
if not target or target == '' then
  return {'0', 'ERR_MAP_NO_LINE', 'map line not found'}
end
local L = load_inst(target)
if not L or not usable(L) then
  if L and (L[7] or '') == 'DRAINING' then
    return {'0', 'ERR_MAP_DRAINING', 'map line draining'}
  end
  return {'0', 'ERR_MAP_NOT_READY', 'map line not joinable'}
end
local ok = occ_key(target)
if redis.call('SISMEMBER', ok, player) == 0 then
  local n = tonumber(redis.call('SCARD', ok)) or 0
  if n >= hard then return {'0', 'ERR_MAP_LINE_FULL', 'map line full'} end
  redis.call('SADD', ok, player)
  redis.call('EXPIRE', ok, 86400)
  redis.call('ZREM', prefix .. 'map:idle', tostring(target))
end
redis.call('SREM', occ_key(old_id), player)
local left = tonumber(redis.call('SCARD', occ_key(old_id))) or 0
if left == 0 then
  redis.call('HSET', prefix .. 'map:inst:' .. old_id, 'lastOccupiedAt', tostring(now))
  redis.call('ZADD', prefix .. 'map:idle', now, old_id)
end
save_pres(target)
local occ = tonumber(redis.call('SCARD', ok)) or 0
local R = pack(L, occ, '0')
cache_op(R)
return R
)LUA";

const char kLuaEnqueueMap[] = R"LUA(
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local player = ARGV[4]
local line = tonumber(ARGV[5]) or 0
local token = ARGV[6] or ''
local now = tonumber(ARGV[7]) or 0
local hard = tonumber(ARGV[8]) or 400
if line < 1 then return {'0', 'ERR_INVALID_ARGUMENT', 'line_no required'} end
if hard < 1 then hard = 1 end

local qkey = prefix .. 'map:queue:' .. realm .. ':' .. tpl .. ':' .. tostring(line)
local pkey = prefix .. 'map:qplayer:' .. player
local lkey = prefix .. 'map:line:' .. realm .. ':' .. tpl .. ':' .. tostring(line)

local function rank_of()
  local r = redis.call('ZRANK', qkey, player)
  if r == false then return nil end
  return tonumber(r)
end

local function ready_of(pos)
  local id = redis.call('GET', lkey)
  if not id or id == '' then return 0 end
  local n = tonumber(redis.call('SCARD', prefix .. 'map:occ:' .. id)) or 0
  if pos == 1 and n < hard then return 1 end
  return 0
end

if token ~= '' then
  local tkey = prefix .. 'map:qtok:' .. token
  local raw = redis.call('HGETALL', tkey)
  if #raw == 0 then return {'0', 'ERR_QUEUE_INVALID', 'queue token invalid'} end
  local f = {}
  for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
  if (f['player'] or '') ~= player then
    return {'0', 'ERR_QUEUE_INVALID', 'queue token player mismatch'}
  end
  local rk = rank_of()
  if rk == nil then return {'0', 'ERR_QUEUE_INVALID', 'not in queue'} end
  local pos = rk + 1
  local len = tonumber(redis.call('ZCARD', qkey)) or 0
  return {'1', 'OK', token, tostring(pos), tostring(len), tostring(line),
          tostring(ready_of(pos))}
end

local existing = redis.call('GET', pkey)
if existing and existing ~= '' then
  token = existing
  local rk = rank_of()
  if rk ~= nil then
    local pos = rk + 1
    local len = tonumber(redis.call('ZCARD', qkey)) or 0
    return {'1', 'OK', token, tostring(pos), tostring(len), tostring(line),
            tostring(ready_of(pos))}
  end
end

token = player .. '-' .. realm .. '-' .. tpl .. '-' .. tostring(line) .. '-' .. tostring(now)
redis.call('ZADD', qkey, now, player)
redis.call('EXPIRE', qkey, 3600)
local tkey = prefix .. 'map:qtok:' .. token
redis.call('HMSET', tkey, 'player', player, 'realmId', realm, 'mapTemplateId', tpl,
           'lineNo', tostring(line), 'issuedAt', tostring(now))
redis.call('EXPIRE', tkey, 3600)
redis.call('SET', pkey, token, 'EX', 3600)
local rk = rank_of() or 0
local pos = rk + 1
local len = tonumber(redis.call('ZCARD', qkey)) or 0
return {'1', 'OK', token, tostring(pos), tostring(len), tostring(line),
        tostring(ready_of(pos))}
)LUA";

const char kLuaDrainMap[] = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local state = f['state'] or ''
if state == 'CLOSED' then return {'0', 'CLOSED', 'closed'} end
if state ~= 'READY' and state ~= 'DRAINING' then
  return {'0', 'BAD_STATE', 'cannot drain'}
end
redis.call('HMSET', key, 'state', 'DRAINING', 'updatedAt', tostring(now),
           'leaseUntil', tostring(now))
redis.call('EXPIRE', key, 86400)
return {'1', 'OK', f['mapInstanceId'] or '0', f['realmId'] or '0',
        f['mapTemplateId'] or '0', f['ownerLogicServerId'] or '',
        f['ownerEpoch'] or '0', f['routeVersion'] or '0', 'DRAINING',
        tostring(now), tostring(now), '0', '0',
        f['kind'] or '', f['lineNo'] or '0', f['softCap'] or '0', f['hardCap'] or '0'}
)LUA";

const char kLuaListLines[] = R"LUA(
local lines_key = KEYS[1]
local prefix = ARGV[1]
local now = tonumber(ARGV[2]) or 0
local ids = redis.call('ZRANGE', lines_key, 0, -1)
local out = {}
for _, id in ipairs(ids) do
  local key = prefix .. 'map:inst:' .. tostring(id)
  local raw = redis.call('HGETALL', key)
  if #raw > 0 then
    local f = {}
    for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
    local occ = tonumber(redis.call('SCARD', prefix .. 'map:occ:' .. tostring(id))) or 0
    out[#out + 1] = tostring(id)
    out[#out + 1] = f['lineNo'] or '0'
    out[#out + 1] = tostring(occ)
    out[#out + 1] = f['softCap'] or '0'
    out[#out + 1] = f['hardCap'] or '0'
    out[#out + 1] = f['state'] or 'CLOSED'
    out[#out + 1] = f['ownerLogicServerId'] or ''
  end
end
return out
)LUA";

const char kLuaReleaseSlot[] = R"LUA(
local pres_key = KEYS[1]
local prefix = ARGV[1]
local player = ARGV[2]
local now = tonumber(ARGV[3]) or 0
local f = {}
local raw = redis.call('HGETALL', pres_key)
if #raw == 0 then return {'1', 'OK', '0'} end
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local inst = f['mapInstanceId'] or '0'
local op = f['operationId'] or ''
if inst ~= '0' and inst ~= '' then
  redis.call('SREM', prefix .. 'map:occ:' .. inst, player)
  local n = tonumber(redis.call('SCARD', prefix .. 'map:occ:' .. inst)) or 0
  if n == 0 then
    local ikey = prefix .. 'map:inst:' .. inst
    redis.call('HSET', ikey, 'lastOccupiedAt', tostring(now))
    redis.call('ZADD', prefix .. 'map:idle', now, inst)
  end
end
redis.call('DEL', pres_key)
if op ~= '' then
  redis.call('DEL', prefix .. 'map:op:' .. player .. ':' .. op)
end
return {'1', 'OK', inst}
)LUA";

const char kLuaCreateDungeon[] = R"LUA(
local idgen_key = KEYS[1]
local op_key = KEYS[2]
local prefix = ARGV[1]
local realm = ARGV[2]
local tpl = ARGV[3]
local leader = ARGV[4]
local op = ARGV[5]
local members_csv = ARGV[6] or ''
local owner = ARGV[7]
local now = tonumber(ARGV[8])
local lease = tonumber(ARGV[9])
local healthy_csv = ARGV[10] or ''
local soft = tonumber(ARGV[11]) or 5
local hard = tonumber(ARGV[12]) or 5
local empty_delay = tonumber(ARGV[13]) or 30
if soft < 1 then soft = 1 end
if hard < soft then hard = soft end
if empty_delay < 1 then empty_delay = 30 end

local function pick_new_owner(new_id)
  local healthy = {}
  for token in string.gmatch(healthy_csv, '[^,]+') do
    healthy[#healthy + 1] = token
  end
  if owner ~= '' then
    for i = 1, #healthy do
      if healthy[i] == owner then
        return owner
      end
    end
  end
  if #healthy == 0 then
    return owner
  end
  local n = tonumber(new_id) or 0
  return healthy[(n % #healthy) + 1]
end

local function pack(L, occ, idem)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9],
          tostring(occ or 0), idem or '0', 'DUNGEON', '0',
          tostring(soft), tostring(hard)}
end

local function cache_op(reply)
  if op == nil or op == '' then return end
  local enc = table.concat(reply, '\t')
  redis.call('SET', op_key, enc, 'EX', 600)
end

if op ~= '' then
  local cached = redis.call('GET', op_key)
  if cached then
    local R = {}
    for token in string.gmatch(cached, '[^\t]+') do
      R[#R + 1] = token
    end
    if #R >= 3 then
      R[13] = '1'
      return R
    end
  end
end

local seen = {}
local members = {}
if leader ~= '' and leader ~= '0' then
  members[#members + 1] = leader
  seen[leader] = true
end
for token in string.gmatch(members_csv, '[^,]+') do
  if token ~= '' and token ~= '0' and not seen[token] then
    members[#members + 1] = token
    seen[token] = true
  end
end
if #members == 0 then
  return {'0', 'INVALID_ARG', 'leader required'}
end
if #members > hard then
  return {'0', 'ERR_DUNGEON_CREATE_FORBIDDEN', 'too many members'}
end

local id = redis.call('INCR', idgen_key)
local chosen = pick_new_owner(id)
local ikey = prefix .. 'map:inst:' .. tostring(id)
local mkey = prefix .. 'map:members:' .. tostring(id)
local lease_until = now + lease
redis.call('HMSET', ikey,
  'mapInstanceId', tostring(id),
  'realmId', realm,
  'mapTemplateId', tpl,
  'ownerLogicServerId', chosen,
  'ownerEpoch', '1',
  'routeVersion', '1',
  'state', 'READY',
  'updatedAt', tostring(now),
  'leaseUntil', tostring(lease_until),
  'kind', 'DUNGEON',
  'lineNo', '0',
  'softCap', tostring(soft),
  'hardCap', tostring(hard),
  'emptyCloseDelay', tostring(empty_delay),
  'lastOccupiedAt', tostring(now))
redis.call('EXPIRE', ikey, 86400)
for _, pid in ipairs(members) do
  redis.call('SADD', mkey, pid)
end
redis.call('EXPIRE', mkey, 86400)
redis.call('ZADD', prefix .. 'map:idle', now, tostring(id))
local L = {tostring(id), realm, tpl, chosen, '1', '1', 'READY', tostring(now),
           tostring(lease_until)}
local R = pack(L, 0, '0')
cache_op(R)
return R
)LUA";

const char kLuaReserveDungeon[] = R"LUA(
local pres_key = KEYS[1]
local op_key = KEYS[2]
local prefix = ARGV[1]
local player = ARGV[2]
local op = ARGV[3]
local want_id = ARGV[4]
local now = tonumber(ARGV[5]) or 0

local function load_inst(id)
  local key = prefix .. 'map:inst:' .. tostring(id)
  local raw = redis.call('HGETALL', key)
  if #raw == 0 then return nil end
  local f = {}
  for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
  return {
    tostring(id), f['realmId'] or '0', f['mapTemplateId'] or '0',
    f['ownerLogicServerId'] or '', f['ownerEpoch'] or '0',
    f['routeVersion'] or '0', f['state'] or 'CLOSED',
    f['updatedAt'] or '0', f['leaseUntil'] or '0',
    f['kind'] or 'DUNGEON',
    tonumber(f['softCap'] or '5') or 5,
    tonumber(f['hardCap'] or '5') or 5
  }
end

local function pack(L, occ, idem)
  return {'1', 'OK', L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[8], L[9],
          tostring(occ or 0), idem or '0', L[10] or 'DUNGEON', '0',
          tostring(L[11] or 5), tostring(L[12] or 5)}
end

local function save_pres(id)
  redis.call('HMSET', pres_key,
    'mapInstanceId', tostring(id),
    'state', 'reserved',
    'operationId', op,
    'kind', 'DUNGEON')
  redis.call('EXPIRE', pres_key, 86400)
end

local function cache_op(reply)
  if op == nil or op == '' then return end
  local enc = table.concat(reply, '\t')
  redis.call('SET', op_key, enc, 'EX', 600)
end

if op ~= '' then
  local cached = redis.call('GET', op_key)
  if cached then
    local R = {}
    for token in string.gmatch(cached, '[^\t]+') do
      R[#R + 1] = token
    end
    if #R >= 3 then
      R[13] = '1'
      return R
    end
  end
end

if want_id == '0' or want_id == '' then
  return {'0', 'ERR_DUNGEON_CREATE_FORBIDDEN', 'create dungeon first'}
end

local L = load_inst(want_id)
if not L then
  return {'0', 'ERR_DUNGEON_NOT_FOUND', 'dungeon not found'}
end
if (L[7] or '') == 'CLOSED' then
  return {'0', 'ERR_DUNGEON_NOT_FOUND', 'dungeon closed'}
end
if (L[10] or '') ~= 'DUNGEON' then
  return {'0', 'ERR_DUNGEON_NOT_FOUND', 'not a dungeon'}
end
if (L[7] or '') ~= 'READY' or (tonumber(L[9]) or 0) <= now then
  return {'0', 'NOT_READY', 'dungeon not joinable'}
end
local mkey = prefix .. 'map:members:' .. tostring(want_id)
if redis.call('SISMEMBER', mkey, player) ~= 1 then
  return {'0', 'ERR_DUNGEON_NOT_MEMBER', 'not a dungeon member'}
end
local occ = prefix .. 'map:occ:' .. tostring(want_id)
if redis.call('SISMEMBER', occ, player) ~= 1 then
  local n = tonumber(redis.call('SCARD', occ)) or 0
  local hard = tonumber(L[12]) or 5
  if n >= hard then
    return {'0', 'ERR_MAP_FULL', 'dungeon full'}
  end
  redis.call('SADD', occ, player)
  redis.call('EXPIRE', occ, 86400)
end
redis.call('ZREM', prefix .. 'map:idle', tostring(want_id))
save_pres(want_id)
local n = tonumber(redis.call('SCARD', occ)) or 0
local R = pack(L, n, '0')
cache_op(R)
return R
)LUA";

const char kLuaCloseIdle[] = R"LUA(
local idle_key = KEYS[1]
local prefix = ARGV[1]
local now = tonumber(ARGV[2]) or 0
local limit = tonumber(ARGV[3]) or 32
if limit < 1 then limit = 1 end
local rows = redis.call('ZRANGE', idle_key, 0, -1, 'WITHSCORES')
local closed = {}
local n = 0
for i = 1, #rows, 2 do
  if n >= limit then break end
  local id = rows[i]
  local last = tonumber(rows[i + 1]) or 0
  local ikey = prefix .. 'map:inst:' .. tostring(id)
  local raw = redis.call('HGETALL', ikey)
  if #raw == 0 then
    redis.call('ZREM', idle_key, id)
  else
    local f = {}
    for j = 1, #raw, 2 do f[raw[j]] = raw[j + 1] end
    local kind = f['kind'] or ''
    local delay = tonumber(f['emptyCloseDelay'] or '0') or 0
    if delay < 1 then
      if kind == 'DUNGEON' then delay = 30 else delay = 300 end
    end
    local occ = tonumber(redis.call('SCARD', prefix .. 'map:occ:' .. tostring(id))) or 0
    if occ > 0 then
      redis.call('ZREM', idle_key, id)
    elseif (f['state'] or '') == 'CLOSED' then
      redis.call('ZREM', idle_key, id)
    elseif now - last >= delay then
      local can_close = true
      if kind == 'LINE' then
        local min_lines = tonumber(f['minLines'] or '1') or 1
        local realm = f['realmId'] or '0'
        local tpl = f['mapTemplateId'] or '0'
        local lines_key = prefix .. 'map:lines:' .. realm .. ':' .. tpl
        local ids = redis.call('ZRANGE', lines_key, 0, -1)
        local live = 0
        for _, lid in ipairs(ids) do
          local st = redis.call('HGET', prefix .. 'map:inst:' .. tostring(lid), 'state')
          if st == 'READY' then live = live + 1 end
        end
        if live <= min_lines then can_close = false end
      end
      if can_close and (kind == 'LINE' or kind == 'DUNGEON') then
        redis.call('HSET', ikey, 'state', 'CLOSED', 'updatedAt', tostring(now))
        if kind == 'LINE' then
          local realm = f['realmId'] or '0'
          local tpl = f['mapTemplateId'] or '0'
          local ln = f['lineNo'] or '0'
          redis.call('DEL', prefix .. 'map:line:' .. realm .. ':' .. tpl .. ':' .. ln)
          redis.call('ZREM', prefix .. 'map:lines:' .. realm .. ':' .. tpl, id)
        end
        redis.call('DEL', prefix .. 'map:members:' .. tostring(id))
        redis.call('ZREM', idle_key, id)
        closed[#closed + 1] = tostring(id)
        n = n + 1
      end
    end
  end
end
return closed
)LUA";

const char kLuaConfirmSlot[] = R"LUA(
local pres_key = KEYS[1]
local want = ARGV[1]
if redis.call('EXISTS', pres_key) == 0 then return {'0', 'NOT_FOUND'} end
local inst = redis.call('HGET', pres_key, 'mapInstanceId') or '0'
if want ~= '0' and want ~= '' and inst ~= want then
  return {'0', 'MISMATCH'}
end
redis.call('HSET', pres_key, 'state', 'confirmed')
return {'1', 'OK', inst}
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
    case PlacementState::Draining:
        return "DRAINING";
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
    if (s == "DRAINING")
        return PlacementState::Draining;
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

void PlacementStore::SetLineStatusHook(LineStatusHook hook) {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    line_status_hook_ = hook;
}

void PlacementStore::FireLineStatus(uint32_t realm_id, uint64_t map_template_id) {
    LineStatusHook hook = nullptr;
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        hook = line_status_hook_;
    }
    if (!hook || map_template_id == 0)
        return;
    hook(realm_id == 0 ? 1 : realm_id, map_template_id);
}

void PlacementStore::SetLogicOwners(std::vector<std::string> owners, bool publish_snapshot) {
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        if (owners_ != owners)
            owners_ = std::move(owners);
        // 不重置 rr_：每次 Discover 清零会把 PickOwner / PickHealthyOwner 钉在 owners[0]
    }
    if (publish_snapshot) {
        auto snap = std::make_shared<HealthyLogicSnapshot>();
        snap->source = HealthyLogicSnapshot::Source::kStatic;
        snap->instance_ids = [&] {
            std::lock_guard<std::mutex> lk(cfg_mu_);
            return owners_;
        }();
        snap->state = snap->instance_ids.empty() ? HealthyLogicSnapshot::State::kEmpty
                                                 : HealthyLogicSnapshot::State::kBootstrap;
        HealthyLogicSnapshotStore::Instance().Publish(snap);
    }
}

std::vector<std::string> PlacementStore::OwnersForPick() const {
    auto snap = HealthyLogicSnapshotStore::Instance().Current();
    if (snap && snap->version > 0)
        return snap->instance_ids;
    std::lock_guard<std::mutex> lk(cfg_mu_);
    return owners_;
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

bool PlacementStore::HasHealthyOwners() const {
    return !OwnersForPick().empty();
}

std::string PlacementStore::CreateOwnerHint(const std::string &preferred) const {
    if (!HasHealthyOwners())
        return {};
    if (preferred.empty())
        return {};
    const auto owners = OwnersForPick();
    for (const auto &o : owners) {
        if (o == preferred)
            return preferred;
    }
    return {};
}

std::string PlacementStore::HealthyOwnersCsv() const {
    const auto owners = OwnersForPick();
    if (owners.empty())
        return {};
    std::ostringstream os;
    for (size_t i = 0; i < owners.size(); ++i) {
        if (i)
            os << ',';
        os << owners[i];
    }
    return os.str();
}

std::string PlacementStore::PoolKey(uint32_t realm, uint64_t tpl) const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%smap:pool:%u:%llu", key_prefix_.c_str(), realm,
                  static_cast<unsigned long long>(tpl));
    return buf;
}

std::string PlacementStore::OccKey(uint64_t map_instance_id) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%smap:occ:%llu", key_prefix_.c_str(),
                  static_cast<unsigned long long>(map_instance_id));
    return buf;
}

std::string PlacementStore::PresKey(uint64_t player_id) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%smap:pres:%llu", key_prefix_.c_str(),
                  static_cast<unsigned long long>(player_id));
    return buf;
}

std::string PlacementStore::OpKey(uint64_t player_id, const std::string &operation_id) const {
    return key_prefix_ + "map:op:" + std::to_string(player_id) + ":" + operation_id;
}

std::string PlacementStore::MembersKey(uint64_t map_instance_id) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%smap:members:%llu", key_prefix_.c_str(),
                  static_cast<unsigned long long>(map_instance_id));
    return buf;
}

std::string PlacementStore::IdleKey() const {
    return key_prefix_ + "map:idle";
}

void PlacementStore::SetPublicMapCapacity(uint32_t n) {
    if (n > 0)
        public_capacity_ = n;
}

bool PlacementStore::ReservePublicSlot(const ResolveOrCreateInput &in, ResolveOrCreateResult *out) {
    if (!out)
        return false;
    *out = ResolveOrCreateResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_template_id == 0) {
        out->message = "player_id and map_template_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    if (!HasHealthyOwners()) {
        out->message = "no healthy gamelogic";
        out->error_code = "NO_HEALTHY_GAMELOGIC";
        return false;
    }
    const std::string owner = CreateOwnerHint(in.preferred_owner);
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const uint32_t cap = in.capacity > 0 ? in.capacity : public_capacity_;
    const std::string op = in.operation_id.empty()
                               ? ("enter:" + std::to_string(in.player_id) + ":" +
                                  std::to_string(in.map_template_id))
                               : in.operation_id;
    std::vector<std::string> keys{PoolKey(in.realm_id, in.map_template_id), IdGenKey(),
                                  PresKey(in.player_id), OpKey(in.player_id, op)};
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        std::to_string(in.player_id),
        op,
        std::to_string(cap),
        std::to_string(in.map_instance_id),
        owner,
        std::to_string(NowUnixSec()),
        std::to_string(default_lease_sec_),
        HealthyOwnersCsv(),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaReserveSlot, keys, args, &reply) || reply.size() < 3) {
        out->message = "reserve lua failed";
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
    if (reply.size() > 11)
        out->occupancy = static_cast<uint32_t>(ParseU64(reply[11]));
    if (reply.size() > 12)
        out->idempotent_hit = (reply[12] == "1");
    out->ok = true;
    out->message = "ok";
    return true;
}

bool PlacementStore::ConfirmSlot(uint64_t player_id, uint64_t map_instance_id) {
    if (!available_ || player_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{PresKey(player_id)};
    std::vector<std::string> args{std::to_string(map_instance_id)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaConfirmSlot, keys, args, &reply) || reply.empty())
        return false;
    return reply[0] == "1";
}

bool PlacementStore::ReleaseByPlayer(uint64_t player_id) {
    if (!available_ || player_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{PresKey(player_id)};
    std::vector<std::string> args{key_prefix_, std::to_string(player_id),
                                  std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaReleaseSlot, keys, args, &reply) || reply.empty())
        return false;
    const bool ok = reply[0] == "1";
    if (ok && reply.size() > 2) {
        const uint64_t inst = ParseU64(reply[2]);
        PlacementRecord rec;
        if (inst != 0 && Get(inst, &rec) && rec.map_template_id != 0)
            FireLineStatus(rec.realm_id, rec.map_template_id);
    }
    return ok;
}

std::string PlacementStore::QueueKey(uint32_t realm, uint64_t tpl, uint32_t line_no) const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%smap:queue:%u:%llu:%u", key_prefix_.c_str(), realm,
                  static_cast<unsigned long long>(tpl), line_no);
    return buf;
}

bool PlacementStore::SwitchLine(const SwitchLineInput &in, ResolveOrCreateResult *out) {
    if (!out)
        return false;
    *out = ResolveOrCreateResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_template_id == 0 || in.line_no == 0) {
        out->message = "player_id, map_template_id and line_no required";
        out->error_code = "ERR_INVALID_ARGUMENT";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const uint32_t soft = in.soft_cap > 0 ? in.soft_cap : 200;
    const uint32_t hard = in.hard_cap > 0 ? in.hard_cap : 400;
    const std::string op = in.operation_id.empty()
                               ? ("switch:" + std::to_string(in.player_id) + ":" +
                                  std::to_string(in.map_template_id) + ":L" +
                                  std::to_string(in.line_no))
                               : in.operation_id;
    std::vector<std::string> keys{PresKey(in.player_id), OpKey(in.player_id, op)};
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        std::to_string(in.player_id),
        op,
        std::to_string(in.line_no),
        std::to_string(soft),
        std::to_string(hard),
        std::to_string(NowUnixSec()),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaSwitchLine, keys, args, &reply) || reply.size() < 3) {
        out->message = "switch line lua failed";
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
    if (reply.size() > 11)
        out->occupancy = static_cast<uint32_t>(ParseU64(reply[11]));
    if (reply.size() > 12)
        out->idempotent_hit = (reply[12] == "1");
    if (out->placement.kind.empty())
        out->placement.kind = "LINE";
    out->ok = true;
    out->message = "ok";
    FireLineStatus(out->placement.realm_id, out->placement.map_template_id);
    return true;
}

bool PlacementStore::EnqueueMap(const EnqueueMapInput &in, EnqueueMapResult *out) {
    if (!out)
        return false;
    *out = EnqueueMapResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_template_id == 0 || in.line_no == 0) {
        out->message = "player_id, map_template_id and line_no required";
        out->error_code = "ERR_INVALID_ARGUMENT";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const uint32_t hard = in.hard_cap > 0 ? in.hard_cap : 400;
    std::vector<std::string> keys;
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        std::to_string(in.player_id),
        std::to_string(in.line_no),
        in.queue_token,
        std::to_string(NowUnixSec()),
        std::to_string(hard),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaEnqueueMap, keys, args, &reply) || reply.size() < 3) {
        out->message = "enqueue lua failed";
        out->error_code = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : "rejected";
        out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
        return false;
    }
    out->ok = true;
    out->message = "ok";
    if (reply.size() > 2)
        out->queue_token = reply[2];
    if (reply.size() > 3)
        out->queue_position = static_cast<uint32_t>(ParseU64(reply[3]));
    if (reply.size() > 4)
        out->queue_length = static_cast<uint32_t>(ParseU64(reply[4]));
    if (reply.size() > 5)
        out->line_no = static_cast<uint32_t>(ParseU64(reply[5]));
    if (reply.size() > 6)
        out->ready = (reply[6] == "1");
    return true;
}

bool PlacementStore::Drain(uint64_t map_instance_id, const std::string &reason,
                           PlacementRecord *out, std::string *err) {
    (void)reason;
    if (!available_ || map_instance_id == 0) {
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
    std::vector<std::string> args{std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaDrainMap, keys, args, &reply) || reply.size() < 3) {
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
    return true;
}

bool PlacementStore::ListOccupants(uint64_t map_instance_id, std::vector<uint64_t> *out) {
    if (!out)
        return false;
    out->clear();
    if (!available_ || map_instance_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> members;
    if (!lease->SMembers(OccKey(map_instance_id), &members))
        return false;
    for (const auto &m : members) {
        const uint64_t pid = ParseU64(m);
        if (pid != 0)
            out->push_back(pid);
    }
    return true;
}

bool PlacementStore::GetPlayerPresence(uint64_t player_id, uint64_t *map_instance_id) {
    if (!map_instance_id)
        return false;
    *map_instance_id = 0;
    if (!available_ || player_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> fields;
    if (!lease->HGetAll(PresKey(player_id), &fields))
        return false;
    auto it = fields.find("mapInstanceId");
    if (it != fields.end())
        *map_instance_id = ParseU64(it->second);
    return true;
}

uint32_t PlacementStore::Occupancy(uint64_t map_instance_id) {
    if (!available_ || map_instance_id == 0)
        return 0;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return 0;
    std::vector<std::string> keys{OccKey(map_instance_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    if (!lease->Eval("return redis.call('SCARD', KEYS[1])", keys, args, &reply) || reply.empty())
        return 0;
    return static_cast<uint32_t>(ParseU64(reply[0]));
}

std::string PlacementStore::LinesKey(uint32_t realm, uint64_t tpl) const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%smap:lines:%u:%llu", key_prefix_.c_str(), realm,
                  static_cast<unsigned long long>(tpl));
    return buf;
}

std::string PlacementStore::LineKey(uint32_t realm, uint64_t tpl, uint32_t line_no) const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%smap:line:%u:%llu:%u", key_prefix_.c_str(), realm,
                  static_cast<unsigned long long>(tpl), line_no);
    return buf;
}

bool PlacementStore::ReserveLine(const ResolveOrCreateInput &in, ResolveOrCreateResult *out) {
    if (!out)
        return false;
    *out = ResolveOrCreateResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_template_id == 0) {
        out->message = "player_id and map_template_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    if (!HasHealthyOwners()) {
        out->message = "no healthy gamelogic";
        out->error_code = "NO_HEALTHY_GAMELOGIC";
        return false;
    }
    const std::string owner = CreateOwnerHint(in.preferred_owner);
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const uint32_t soft = in.soft_cap > 0 ? in.soft_cap : 200;
    const uint32_t hard = in.hard_cap > 0 ? in.hard_cap : 400;
    const uint32_t max_lines = in.max_lines > 0 ? in.max_lines : 8;
    const std::string op = in.operation_id.empty()
                               ? ("enter:" + std::to_string(in.player_id) + ":" +
                                  std::to_string(in.map_template_id) + ":L" +
                                  std::to_string(in.line_no))
                               : in.operation_id;
    std::vector<std::string> keys{LinesKey(in.realm_id, in.map_template_id), IdGenKey(),
                                  PresKey(in.player_id), OpKey(in.player_id, op)};
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        std::to_string(in.player_id),
        op,
        std::to_string(soft),
        std::to_string(hard),
        std::to_string(max_lines),
        std::to_string(in.map_instance_id),
        std::to_string(in.line_no),
        owner,
        std::to_string(NowUnixSec()),
        std::to_string(default_lease_sec_),
        HealthyOwnersCsv(),
        std::to_string(in.empty_close_delay > 0 ? in.empty_close_delay : 300),
        std::to_string(in.min_lines > 0 ? in.min_lines : 1),
        in.queue_token,
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaReserveLine, keys, args, &reply) || reply.size() < 3) {
        out->message = "reserve line lua failed";
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
    if (reply.size() > 11)
        out->occupancy = static_cast<uint32_t>(ParseU64(reply[11]));
    if (reply.size() > 12)
        out->idempotent_hit = (reply[12] == "1");
    if (out->placement.kind.empty())
        out->placement.kind = "LINE";
    out->ok = true;
    out->message = "ok";
    FireLineStatus(out->placement.realm_id, out->placement.map_template_id);
    return true;
}

bool PlacementStore::ListLines(uint32_t realm_id, uint64_t map_template_id,
                               std::vector<MapLineInfo> *out) {
    if (!out || !available_ || map_template_id == 0)
        return false;
    out->clear();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{LinesKey(realm_id, map_template_id)};
    std::vector<std::string> args{key_prefix_, std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaListLines, keys, args, &reply))
        return false;
    for (size_t i = 0; i + 5 < reply.size(); i += 7) {
        MapLineInfo row;
        row.map_instance_id = ParseU64(reply[i]);
        row.line_no = static_cast<uint32_t>(ParseU64(reply[i + 1]));
        row.occupancy = static_cast<uint32_t>(ParseU64(reply[i + 2]));
        row.soft_cap = static_cast<uint32_t>(ParseU64(reply[i + 3]));
        row.hard_cap = static_cast<uint32_t>(ParseU64(reply[i + 4]));
        row.state = reply[i + 5];
        if (i + 6 < reply.size())
            row.owner_logic_server_id = reply[i + 6];
        out->push_back(row);
    }
    return true;
}

bool PlacementStore::CreateDungeon(const CreateDungeonInput &in, CreateDungeonResult *out) {
    if (!out)
        return false;
    *out = CreateDungeonResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_template_id == 0) {
        out->message = "player_id and map_template_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    if (!HasHealthyOwners()) {
        out->message = "no healthy gamelogic";
        out->error_code = "NO_HEALTHY_GAMELOGIC";
        return false;
    }
    const std::string owner = CreateOwnerHint(in.preferred_owner);
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const uint32_t soft = in.soft_cap > 0 ? in.soft_cap : 5;
    const uint32_t hard = in.hard_cap > 0 ? in.hard_cap : 5;
    const uint32_t delay = in.empty_close_delay > 0 ? in.empty_close_delay : 30;
    const std::string op = in.operation_id.empty()
                               ? ("dungeon:" + std::to_string(in.player_id) + ":" +
                                  std::to_string(in.map_template_id))
                               : in.operation_id;
    std::ostringstream mem;
    bool first = true;
    for (uint64_t pid : in.member_player_ids) {
        if (pid == 0)
            continue;
        if (!first)
            mem << ',';
        mem << pid;
        first = false;
    }
    std::vector<std::string> keys{IdGenKey(), OpKey(in.player_id, op)};
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.realm_id),
        std::to_string(in.map_template_id),
        std::to_string(in.player_id),
        op,
        mem.str(),
        owner,
        std::to_string(NowUnixSec()),
        std::to_string(default_lease_sec_),
        HealthyOwnersCsv(),
        std::to_string(soft),
        std::to_string(hard),
        std::to_string(delay),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaCreateDungeon, keys, args, &reply) || reply.size() < 3) {
        out->message = "create dungeon lua failed";
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
    if (reply.size() > 12)
        out->idempotent_hit = (reply[12] == "1");
    if (out->placement.kind.empty())
        out->placement.kind = "DUNGEON";
    std::vector<std::string> members;
    if (lease->SMembers(MembersKey(out->placement.map_instance_id), &members)) {
        for (const auto &s : members)
            out->member_player_ids.push_back(ParseU64(s));
    }
    out->ok = true;
    out->message = "ok";
    return true;
}

bool PlacementStore::ReserveDungeonEnter(const ResolveOrCreateInput &in,
                                         ResolveOrCreateResult *out) {
    if (!out)
        return false;
    *out = ResolveOrCreateResult{};
    if (!available_) {
        out->message = "placement store unavailable";
        out->error_code = "UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.map_instance_id == 0) {
        out->message = "player_id and map_instance_id required";
        out->error_code = "ERR_DUNGEON_CREATE_FORBIDDEN";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    const std::string op = in.operation_id.empty()
                               ? ("dungeon-enter:" + std::to_string(in.player_id) + ":" +
                                  std::to_string(in.map_instance_id))
                               : in.operation_id;
    std::vector<std::string> keys{PresKey(in.player_id), OpKey(in.player_id, op)};
    std::vector<std::string> args{
        key_prefix_,
        std::to_string(in.player_id),
        op,
        std::to_string(in.map_instance_id),
        std::to_string(NowUnixSec()),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaReserveDungeon, keys, args, &reply) || reply.size() < 3) {
        out->message = "reserve dungeon lua failed";
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
    if (reply.size() > 11)
        out->occupancy = static_cast<uint32_t>(ParseU64(reply[11]));
    if (reply.size() > 12)
        out->idempotent_hit = (reply[12] == "1");
    if (out->placement.kind.empty())
        out->placement.kind = "DUNGEON";
    out->ok = true;
    out->message = "ok";
    return true;
}

bool PlacementStore::CloseIdleInstances(int64_t now_unix, size_t limit,
                                        std::vector<uint64_t> *closed) {
    if (!available_)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    const int64_t now = now_unix > 0 ? now_unix : NowUnixSec();
    const size_t n = limit > 0 ? limit : 32;
    std::vector<std::string> keys{IdleKey()};
    std::vector<std::string> args{key_prefix_, std::to_string(now), std::to_string(n)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaCloseIdle, keys, args, &reply))
        return false;
    if (closed)
        closed->clear();
    for (const auto &s : reply) {
        const uint64_t id = ParseU64(s);
        if (id == 0)
            continue;
        if (closed)
            closed->push_back(id);
        PlacementRecord rec;
        if (Get(id, &rec) && rec.map_template_id != 0)
            FireLineStatus(rec.realm_id, rec.map_template_id);
    }
    return true;
}

bool PlacementStore::ResolveOrCreate(const ResolveOrCreateInput &in, ResolveOrCreateResult *out) {
    if (in.player_id != 0) {
        if (IsDungeonKind(in.kind)) {
            if (in.map_instance_id == 0) {
                if (!out)
                    return false;
                *out = ResolveOrCreateResult{};
                out->message = "create dungeon first";
                out->error_code = "ERR_DUNGEON_CREATE_FORBIDDEN";
                return false;
            }
            return ReserveDungeonEnter(in, out);
        }
        if (IsLineKind(in.kind))
            return ReserveLine(in, out);
        return ReservePublicSlot(in, out);
    }
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
    if (!HasHealthyOwners()) {
        out->message = "no healthy gamelogic";
        out->error_code = "NO_HEALTHY_GAMELOGIC";
        return false;
    }
    const std::string owner = CreateOwnerHint(in.preferred_owner);
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
    out->kind = fields["kind"];
    out->line_no = static_cast<uint32_t>(ParseU64(fields["lineNo"]));
    out->soft_cap = static_cast<uint32_t>(ParseU64(fields["softCap"]));
    out->hard_cap = static_cast<uint32_t>(ParseU64(fields["hardCap"]));
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
    const auto owners = OwnersForPick();
    if (owners.empty())
        return {};
    std::lock_guard<std::mutex> lk(cfg_mu_);
    for (size_t i = 0; i < owners.size(); ++i) {
        const size_t idx = (rr_ + i) % owners.size();
        if (owners[idx] != exclude)
            return owners[idx];
    }
    return {};
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
