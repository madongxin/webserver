#include "SessionStore.h"

#include "HealthyLogicSnapshot.h"
#include "Logging.h"
#include "PlacementStore.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"
#include "SecureRandom.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

namespace {

int64_t NowUnixSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string GenHex(size_t n) {
    std::string s;
    if (!SecureRandom::Hex(n, &s))
        return {};
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool ParseConfig(const std::string &path, std::string *host, int *port, std::string *password,
                 int *ttl, int *long_ttl, int *grace, int *pool_size, std::string *key_prefix) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "ip")
            *host = val;
        else if (key == "port")
            *port = std::atoi(val.c_str());
        else if (key == "password")
            *password = val;
        else if (key == "session_ttl_sec")
            *ttl = std::atoi(val.c_str());
        else if (key == "session_ttl_long_sec")
            *long_ttl = std::atoi(val.c_str());
        else if (key == "session_grace_sec")
            *grace = std::atoi(val.c_str());
        else if (key == "redis_pool_size")
            *pool_size = std::atoi(val.c_str());
        else if (key == "key_prefix" && key_prefix)
            *key_prefix = val;
    }
    return true;
}

// AcquireSession：原子踢号/建会话/递增 generation
const char kLuaAcquire[] = R"LUA(
local key = KEYS[1]
local kick = tonumber(ARGV[1])
local device = ARGV[2]
local token = ARGV[3]
local session_id = ARGV[4]
local server_id = ARGV[5]
local login_time = ARGV[6]
local gateway_id = ARGV[7]
local logic_id = ARGV[8]
local map_id = ARGV[9]
local map_epoch = ARGV[10]
local route_version = ARGV[11]
local ttl = tonumber(ARGV[12])
local now = tonumber(ARGV[13])
local raw = redis.call('HGETALL', key)
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local state = f['state']
local deadline = tonumber(f['disconnectDeadline'] or '0') or 0
if state == 'DISCONNECTED' and deadline > 0 and now > deadline then
  redis.call('DEL', key)
  f = {}
  state = nil
end
local old_gen = tonumber(f['generation'] or '0') or 0
local old_gw = f['gatewayId'] or ''
local old_sid = f['sessionId'] or ''
local old_token = f['token'] or ''
local old_device = f['deviceId'] or ''
local old_logic = f['gamelogicInstanceId'] or ''
local old_map = f['mapInstanceId'] or '0'
local old_epoch = f['mapOwnerEpoch'] or '0'
local old_rv = f['routeVersion'] or '0'
local old_server = f['serverId'] or '0'
local old_login = f['loginTime'] or '0'
local kicked = 0
if state ~= nil then
  kicked = 1
  if state == 'ONLINE' and kick == 0 and (f['deviceId'] or '') ~= device then
    return {'0', 'ALREADY_ONLINE', 'already logged in on another device'}
  end
end
local gen = 1
if old_gen > 0 then gen = old_gen + 1 end
redis.call('HMSET', key,
  'token', token,
  'sessionId', session_id,
  'serverId', server_id,
  'loginTime', login_time,
  'deviceId', device,
  'state', 'ONLINE',
  'gatewayId', gateway_id,
  'connectionId', '0',
  'generation', tostring(gen),
  'disconnectDeadline', '0',
  'gamelogicInstanceId', logic_id,
  'mapInstanceId', map_id,
  'mapOwnerEpoch', map_epoch,
  'routeVersion', route_version)
redis.call('EXPIRE', key, ttl)
return {'1', 'OK', tostring(gen), route_version, tostring(kicked), token, session_id, logic_id, map_id, map_epoch,
        old_gw, old_sid, tostring(old_gen), old_token, old_device, old_logic, old_map, old_epoch, old_rv,
        old_server, old_login}
)LUA";

// 幂等操作：PENDING / DONE|payload；同 operation_id 超时重试返回同一结果
const char kLuaOpBegin[] = R"LUA(
local key = KEYS[1]
local ttl = tonumber(ARGV[1]) or 120
local v = redis.call('GET', key)
if v then
  if string.sub(v, 1, 5) == 'DONE|' then return {'DONE', v} end
  if v == 'PENDING' then return {'PENDING'} end
end
if redis.call('SET', key, 'PENDING', 'NX', 'EX', ttl) then
  return {'EXECUTE'}
end
v = redis.call('GET', key)
if v and string.sub(v, 1, 5) == 'DONE|' then return {'DONE', v} end
return {'PENDING'}
)LUA";

const char kLuaOpComplete[] = R"LUA(
local key = KEYS[1]
local payload = ARGV[1]
local ttl = tonumber(ARGV[2]) or 86400
redis.call('SET', key, payload, 'EX', ttl)
return {'1'}
)LUA";

const char kLuaOpAbort[] = R"LUA(
local key = KEYS[1]
if redis.call('GET', key) == 'PENDING' then
  redis.call('DEL', key)
end
return {'1'}
)LUA";

const char kLuaRestorePrevious[] = R"LUA(
local key = KEYS[1]
local expect_new = ARGV[1]
local old_token = ARGV[2]
local old_sid = ARGV[3]
local old_gen = ARGV[4]
local old_gw = ARGV[5]
local old_device = ARGV[6]
local old_logic = ARGV[7]
local old_map = ARGV[8]
local old_epoch = ARGV[9]
local old_rv = ARGV[10]
local old_server = ARGV[11]
local old_login = ARGV[12]
local ttl = tonumber(ARGV[13]) or 7200
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= expect_new then
  return {'0', 'NOT_OWNER'}
end
if old_token == '' or old_sid == '' then
  redis.call('DEL', key)
  return {'1', 'DELETED'}
end
redis.call('HMSET', key,
  'token', old_token,
  'sessionId', old_sid,
  'generation', old_gen,
  'gatewayId', old_gw,
  'deviceId', old_device,
  'gamelogicInstanceId', old_logic,
  'mapInstanceId', old_map,
  'mapOwnerEpoch', old_epoch,
  'routeVersion', old_rv,
  'serverId', old_server,
  'loginTime', old_login,
  'state', 'ONLINE',
  'connectionId', '0',
  'disconnectDeadline', '0')
redis.call('EXPIRE', key, ttl)
return {'1', 'RESTORED'}
)LUA";

const char kLuaOpInvalidate[] = R"LUA(
local key = KEYS[1]
redis.call('DEL', key)
return {'1'}
)LUA";

const char kLuaReconnect[] = R"LUA(
local key = KEYS[1]
local sid = ARGV[1]
local ticket = ARGV[2]
local new_token = ARGV[3]
local ttl = tonumber(ARGV[4])
local now = tonumber(ARGV[5])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local state = f['state'] or 'ONLINE'
local deadline = tonumber(f['disconnectDeadline'] or '0') or 0
if state == 'DISCONNECTED' and deadline > 0 and now > deadline then
  redis.call('DEL', key)
  return {'0', 'GRACE_EXPIRED', 'reconnect grace expired'}
end
if state ~= 'DISCONNECTED' and state ~= 'ONLINE' then
  return {'0', 'BAD_STATE', 'session not reconnectable'}
end
if (f['sessionId'] or '') ~= sid then
  return {'0', 'SID_MISMATCH', 'session_id mismatch'}
end
if (f['token'] or '') ~= ticket then
  return {'0', 'TICKET_MISMATCH', 'reconnect_ticket mismatch'}
end
local gen = (tonumber(f['generation'] or '0') or 0) + 1
local rv = (tonumber(f['routeVersion'] or '0') or 0) + 1
redis.call('HMSET', key,
  'token', new_token,
  'generation', tostring(gen),
  'state', 'ONLINE',
  'disconnectDeadline', '0',
  'connectionId', '0',
  'gatewayId', '',
  'routeVersion', tostring(rv))
redis.call('EXPIRE', key, ttl)
return {'1', 'OK', new_token, f['sessionId'] or '', tostring(gen),
        f['gamelogicInstanceId'] or '', f['mapInstanceId'] or '0',
        f['mapOwnerEpoch'] or '0', tostring(rv)}
)LUA";

const char kLuaPrepareReconnect[] = R"LUA(
local sess = KEYS[1]
local pend = KEYS[2]
local sid = ARGV[1]
local ticket = ARGV[2]
local cand = ARGV[3]
local pend_ttl = tonumber(ARGV[4]) or 15
local now = tonumber(ARGV[5]) or 0
local opid = ARGV[6]
local rawp = redis.call('HGETALL', pend)
if #rawp > 0 then
  local p = {}
  for i = 1, #rawp, 2 do p[rawp[i]] = rawp[i + 1] end
  if (p['sessionId'] or '') == sid and (p['oldToken'] or '') == ticket then
    return {'1', 'IDEMPOTENT', p['candidateToken'] or cand, p['candidateGeneration'] or '0',
            p['sessionId'] or sid, p['logic'] or '', p['mapInstanceId'] or '0',
            p['mapOwnerEpoch'] or '0', p['routeVersion'] or '0', p['oldGeneration'] or '0'}
  end
  return {'0', 'OP_CONFLICT', 'pending operation exists'}
end
local raw = redis.call('HGETALL', sess)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local state = f['state'] or 'ONLINE'
local deadline = tonumber(f['disconnectDeadline'] or '0') or 0
if state == 'DISCONNECTED' and deadline > 0 and now > deadline then
  redis.call('DEL', sess)
  return {'0', 'GRACE_EXPIRED', 'reconnect grace expired'}
end
if state ~= 'DISCONNECTED' and state ~= 'ONLINE' then
  return {'0', 'BAD_STATE', 'session not reconnectable'}
end
if (f['sessionId'] or '') ~= sid then
  return {'0', 'SID_MISMATCH', 'session_id mismatch'}
end
if (f['token'] or '') ~= ticket then
  return {'0', 'TICKET_MISMATCH', 'reconnect_ticket mismatch'}
end
local gen = (tonumber(f['generation'] or '0') or 0) + 1
redis.call('HMSET', pend,
  'sessionId', sid,
  'oldToken', ticket,
  'oldGeneration', f['generation'] or '0',
  'candidateToken', cand,
  'candidateGeneration', tostring(gen),
  'logic', f['gamelogicInstanceId'] or '',
  'mapInstanceId', f['mapInstanceId'] or '0',
  'mapOwnerEpoch', f['mapOwnerEpoch'] or '0',
  'routeVersion', f['routeVersion'] or '0',
  'operationId', opid,
  'state', 'PREPARED')
redis.call('EXPIRE', pend, pend_ttl)
return {'1', 'OK', cand, tostring(gen), sid, f['gamelogicInstanceId'] or '',
        f['mapInstanceId'] or '0', f['mapOwnerEpoch'] or '0', f['routeVersion'] or '0',
        f['generation'] or '0'}
)LUA";

const char kLuaCommitReconnect[] = R"LUA(
local sess = KEYS[1]
local pend = KEYS[2]
local sid = ARGV[1]
local opid = ARGV[2]
local cand = ARGV[3]
local cand_gen = ARGV[4]
local ttl = tonumber(ARGV[5]) or 7200
local gw = ARGV[6]
local conn = ARGV[7]
local rawp = redis.call('HGETALL', pend)
if #rawp == 0 then return {'0', 'NO_PENDING', 'pending reconnect not found'} end
local p = {}
for i = 1, #rawp, 2 do p[rawp[i]] = rawp[i + 1] end
if (p['operationId'] or '') ~= opid then return {'0', 'OP_MISMATCH', 'operation_id mismatch'} end
if (p['sessionId'] or '') ~= sid then return {'0', 'SID_MISMATCH', 'session_id mismatch'} end
if (p['candidateToken'] or '') ~= cand then return {'0', 'FENCE_MISMATCH', 'candidate fence mismatch'} end
if (p['candidateGeneration'] or '') ~= cand_gen then return {'0', 'GEN_MISMATCH', 'candidate generation mismatch'} end
local raw = redis.call('HGETALL', sess)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['sessionId'] or '') ~= sid then return {'0', 'SID_MISMATCH', 'session changed'} end
if (f['token'] or '') ~= (p['oldToken'] or '') then return {'0', 'STALE', 'old fence changed'} end
if (f['generation'] or '') ~= (p['oldGeneration'] or '') then return {'0', 'STALE', 'old generation changed'} end
local state = f['state'] or ''
if state ~= 'DISCONNECTED' and state ~= 'ONLINE' then
  return {'0', 'BAD_STATE', 'session not reconnectable'}
end
local rv = (tonumber(f['routeVersion'] or '0') or 0) + 1
redis.call('HMSET', sess,
  'token', cand,
  'generation', cand_gen,
  'state', 'ONLINE',
  'disconnectDeadline', '0',
  'connectionId', conn,
  'gatewayId', gw,
  'routeVersion', tostring(rv))
redis.call('EXPIRE', sess, ttl)
redis.call('DEL', pend)
return {'1', 'OK', cand, sid, cand_gen, p['logic'] or '', p['mapInstanceId'] or '0',
        p['mapOwnerEpoch'] or '0', tostring(rv)}
)LUA";

const char kLuaAbortReconnect[] = R"LUA(
local pend = KEYS[1]
local opid = ARGV[1]
local cand = ARGV[2]
local rawp = redis.call('HGETALL', pend)
if #rawp == 0 then return {'1', 'NOOP'} end
local p = {}
for i = 1, #rawp, 2 do p[rawp[i]] = rawp[i + 1] end
if (p['operationId'] or '') ~= opid then return {'0', 'OP_MISMATCH'} end
if cand ~= '' and (p['candidateToken'] or '') ~= cand then return {'0', 'FENCE_MISMATCH'} end
redis.call('DEL', pend)
return {'1', 'OK'}
)LUA";

const char kLuaMarkDisconnected[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local gen = ARGV[2]
local grace = tonumber(ARGV[3])
local now = tonumber(ARGV[4])
local ttl = tonumber(ARGV[5])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token or (f['generation'] or '') ~= gen then
  return {'0', 'STALE'}
end
local state = f['state'] or 'ONLINE'
if state ~= 'ONLINE' then
  return {'1', 'NOOP'}
end
redis.call('HMSET', key,
  'state', 'DISCONNECTED',
  'disconnectDeadline', tostring(now + grace),
  'connectionId', '0')
redis.call('EXPIRE', key, ttl)
return {'1', 'OK', tostring(now + grace)}
)LUA";

const char kLuaKick[] = R"LUA(
local key = KEYS[1]
local new_token = ARGV[1]
local ttl = tonumber(ARGV[2]) or 7200
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
local gen = (tonumber(f['generation'] or '0') or 0) + 1
redis.call('HMSET', key,
  'token', new_token,
  'generation', tostring(gen),
  'connectionId', '0')
redis.call('EXPIRE', key, ttl)
return {'1', 'OK', tostring(gen), f['gatewayId'] or '', f['token'] or '', f['sessionId'] or '',
        tostring((tonumber(f['generation'] or '0') or 0))}
)LUA";

const char kLuaLogout[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'1', 'ALREADY_OFFLINE'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if token ~= '' and (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH', 'token mismatch'}
end
redis.call('DEL', key)
return {'1', 'OK'}
)LUA";

const char kLuaUpdatePlayerRoute[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local logic = ARGV[2]
local map_id = ARGV[3]
local epoch = ARGV[4]
local want_rv = ARGV[5]
local gateway = ARGV[6]
local push = ARGV[7]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH', 'fence mismatch'}
end
local cur = tonumber(f['routeVersion'] or '0') or 0
local new_rv = tonumber(want_rv) or 0
if new_rv == 0 then new_rv = cur + 1 end
if new_rv < cur then
  return {'0', 'STALE_ROUTE', 'route_version stale'}
end
redis.call('HMSET', key,
  'gamelogicInstanceId', logic,
  'mapInstanceId', map_id,
  'mapOwnerEpoch', epoch,
  'routeVersion', tostring(new_rv))
if gateway ~= '' then redis.call('HSET', key, 'gatewayId', gateway) end
if push ~= '' then redis.call('HSET', key, 'pushEndpoint', push) end
return {'1', 'OK', tostring(new_rv)}
)LUA";

const char kLuaBeginPlayerTransfer[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local expect_rv = tonumber(ARGV[2]) or 0
local from_logic = ARGV[3]
local to_logic = ARGV[4]
local map_id = ARGV[5]
local epoch = ARGV[6]
local tid = ARGV[7]
local gateway = ARGV[8]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH', 'fence mismatch'}
end
if (f['state'] or '') ~= 'ONLINE' and (f['state'] or '') ~= 'TRANSFERRING' then
  return {'0', 'BAD_STATE', 'session not ONLINE'}
end
local cur = tonumber(f['routeVersion'] or '0') or 0
if expect_rv ~= 0 and expect_rv ~= cur then
  return {'0', 'STALE_ROUTE', 'route_version mismatch', tostring(cur)}
end
if (f['routeState'] or '') == 'TRANSFERRING' then
  if tid ~= '' and (f['transferId'] or '') == tid then
    return {'1', 'IDEMPOTENT', tid, tostring(cur), 'TRANSFERRING'}
  end
  return {'0', 'IN_TRANSFER', 'already transferring', f['transferId'] or ''}
end
if from_logic ~= '' and (f['gamelogicInstanceId'] or '') ~= '' and
   (f['gamelogicInstanceId'] or '') ~= from_logic then
  return {'0', 'FROM_MISMATCH', 'from logic mismatch'}
end
if tid == '' then tid = to_logic .. ':' .. map_id .. ':' .. tostring(cur + 1) end
redis.call('HMSET', key,
  'routeState', 'TRANSFERRING',
  'transferId', tid,
  'transferToLogic', to_logic,
  'transferMapId', map_id,
  'transferEpoch', epoch)
if gateway ~= '' then redis.call('HSET', key, 'gatewayId', gateway) end
return {'1', 'OK', tid, tostring(cur), 'TRANSFERRING'}
)LUA";

const char kLuaCommitPlayerTransfer[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local tid = ARGV[2]
local to_logic = ARGV[3]
local map_id = ARGV[4]
local epoch = ARGV[5]
local gateway = ARGV[6]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH', 'fence mismatch'}
end
if (f['routeState'] or '') ~= 'TRANSFERRING' then
  -- 幂等：已提交且目标一致
  if (f['gamelogicInstanceId'] or '') == to_logic and (f['mapInstanceId'] or '') == map_id then
    return {'1', 'IDEMPOTENT', tostring(f['routeVersion'] or '0'), to_logic, map_id, epoch, 'ONLINE'}
  end
  return {'0', 'NOT_TRANSFERRING', 'not in TRANSFERRING'}
end
if (f['transferId'] or '') ~= tid then
  return {'0', 'TRANSFER_MISMATCH', 'transfer_id mismatch'}
end
local cur = tonumber(f['routeVersion'] or '0') or 0
local new_rv = cur + 1
redis.call('HMSET', key,
  'gamelogicInstanceId', to_logic,
  'mapInstanceId', map_id,
  'mapOwnerEpoch', epoch,
  'routeVersion', tostring(new_rv),
  'routeState', 'ONLINE')
redis.call('HDEL', key, 'transferId', 'transferToLogic', 'transferMapId', 'transferEpoch')
if gateway ~= '' then redis.call('HSET', key, 'gatewayId', gateway) end
return {'1', 'OK', tostring(new_rv), to_logic, map_id, epoch, 'ONLINE'}
)LUA";

const char kLuaAbortPlayerTransfer[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local tid = ARGV[2]
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND', 'session not found'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH', 'fence mismatch'}
end
if (f['routeState'] or '') ~= 'TRANSFERRING' then
  return {'1', 'IDEMPOTENT', tostring(f['routeVersion'] or '0'), f['routeState'] or 'ONLINE'}
end
if tid ~= '' and (f['transferId'] or '') ~= tid then
  return {'0', 'TRANSFER_MISMATCH', 'transfer_id mismatch'}
end
redis.call('HSET', key, 'routeState', 'ONLINE')
redis.call('HDEL', key, 'transferId', 'transferToLogic', 'transferMapId', 'transferEpoch')
return {'1', 'OK', tostring(f['routeVersion'] or '0'), 'ONLINE'}
)LUA";

const char kLuaBindConnection[] = R"LUA(
local key = KEYS[1]
local token = ARGV[1]
local gateway_id = ARGV[2]
local connection_id = ARGV[3]
local ttl = tonumber(ARGV[4])
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'0', 'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['token'] or '') ~= token then
  return {'0', 'TOKEN_MISMATCH'}
end
redis.call('HMSET', key,
  'gatewayId', gateway_id,
  'connectionId', connection_id,
  'state', 'ONLINE',
  'disconnectDeadline', '0')
redis.call('EXPIRE', key, ttl)
return {'1', 'OK'}
)LUA";

uint64_t ParseU64(const std::string &s) {
    return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

int64_t ParseI64(const std::string &s) {
    return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10));
}

}  // namespace

SessionStore &SessionStore::Instance() {
    static SessionStore g;
    return g;
}

std::string SessionStore::StateToString(SessionState s) {
    switch (s) {
    case SessionState::Online:
        return "ONLINE";
    case SessionState::Disconnected:
        return "DISCONNECTED";
    case SessionState::Closing:
        return "CLOSING";
    default:
        return "OFFLINE";
    }
}

SessionState SessionStore::StateFromString(const std::string &s) {
    if (s == "ONLINE")
        return SessionState::Online;
    if (s == "DISCONNECTED")
        return SessionState::Disconnected;
    if (s == "CLOSING")
        return SessionState::Closing;
    return SessionState::Offline;
}

bool SessionStore::InitFromConfig() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int ttl = 7200;
    int long_ttl = 86400;
    int grace = 45;
    int pool_size = 8;
    std::string key_prefix = key_prefix_;
    if (!ParseConfig(path, &host, &port, &password, &ttl, &long_ttl, &grace, &pool_size,
                     &key_prefix)) {
        LOG_ERROR << "SessionStore: cannot read " << path;
        available_ = false;
        return false;
    }
    if (ttl > 0)
        default_ttl_sec_ = ttl;
    if (long_ttl > 0)
        long_ttl_sec_ = long_ttl;
    if (grace > 0)
        grace_sec_ = grace;
    if (pool_size > 0)
        pool_size_ = pool_size;
    if (!key_prefix.empty()) {
        if (key_prefix.back() != ':')
            key_prefix.push_back(':');
        key_prefix_ = key_prefix;
    }
    if (!RedisPool::Instance().Init(host, port, password, pool_size_)) {
        LOG_ERROR << "SessionStore: RedisPool init failed " << host << ":" << port;
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "SessionStore: Redis ok " << host << ":" << port
             << " pool=" << pool_size_ << " prefix=" << key_prefix_
             << " default_ttl_sec=" << default_ttl_sec_ << " grace_sec=" << grace_sec_;
    return true;
}

std::string SessionStore::SessionKey(uint64_t player_id) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ssession:%llu", key_prefix_.c_str(),
                  static_cast<unsigned long long>(player_id));
    return buf;
}

std::string SessionStore::OpKey(const std::string &operation_id) const {
    return key_prefix_ + "sessop:" + operation_id;
}

std::string SessionStore::ReconnectPendingKey(const std::string &operation_id) const {
    return key_prefix_ + "reconnect:pending:" + operation_id;
}

namespace {

std::string SanitizeOpField(std::string s) {
    for (char &c : s) {
        if (c == '|')
            c = ' ';
    }
    return s;
}

std::string PackOpResult(const std::string &kind, const AcquireSessionResult &r) {
    std::ostringstream oss;
    oss << "DONE|" << SanitizeOpField(kind) << '|' << (r.ok ? '1' : '0') << '|'
        << SanitizeOpField(r.message) << '|' << SanitizeOpField(r.error_code) << '|'
        << SanitizeOpField(r.session_id) << '|' << SanitizeOpField(r.fence_token) << '|'
        << r.generation << '|' << SanitizeOpField(r.gamelogic_instance_id) << '|'
        << r.map_instance_id << '|' << r.map_owner_epoch << '|' << r.route_version << '|'
        << (r.kicked_previous ? '1' : '0') << '|' << r.login_time_sec << '|' << r.server_id
        << '|' << SanitizeOpField(r.previous_gateway_instance_id) << '|'
        << SanitizeOpField(r.previous_session_id) << '|' << r.previous_generation << '|'
        << SanitizeOpField(r.previous_fence_token) << '|' << SanitizeOpField(r.previous_device_id)
        << '|' << SanitizeOpField(r.previous_gamelogic_instance_id) << '|'
        << r.previous_map_instance_id << '|' << r.previous_map_owner_epoch << '|'
        << r.previous_route_version << '|' << r.previous_server_id << '|'
        << r.previous_login_time_sec;
    return oss.str();
}

uint64_t OpParseU64(const std::string &s) {
    return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

int64_t OpParseI64(const std::string &s) {
    return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10));
}

bool UnpackOpResult(const std::string &packed, std::string *kind, AcquireSessionResult *out) {
    if (!out || packed.size() < 6 || packed.compare(0, 5, "DONE|") != 0)
        return false;
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 5; i < packed.size(); ++i) {
        if (packed[i] == '|') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(packed[i]);
        }
    }
    parts.push_back(cur);
    if (parts.size() < 14)
        return false;
    if (kind)
        *kind = parts[0];
    *out = AcquireSessionResult{};
    out->ok = parts[1] == "1";
    out->message = parts[2];
    out->error_code = parts[3];
    out->session_id = parts[4];
    out->fence_token = parts[5];
    out->generation = OpParseU64(parts[6]);
    out->gamelogic_instance_id = parts[7];
    out->map_instance_id = OpParseU64(parts[8]);
    out->map_owner_epoch = OpParseU64(parts[9]);
    out->route_version = OpParseU64(parts[10]);
    out->kicked_previous = parts[11] == "1";
    out->login_time_sec = OpParseI64(parts[12]);
    out->server_id = static_cast<uint32_t>(OpParseU64(parts[13]));
    if (parts.size() >= 24) {
        out->previous_gateway_instance_id = parts[14];
        out->previous_session_id = parts[15];
        out->previous_generation = OpParseU64(parts[16]);
        out->previous_fence_token = parts[17];
        out->previous_device_id = parts[18];
        out->previous_gamelogic_instance_id = parts[19];
        out->previous_map_instance_id = OpParseU64(parts[20]);
        out->previous_map_owner_epoch = OpParseU64(parts[21]);
        out->previous_route_version = OpParseU64(parts[22]);
        out->previous_server_id = static_cast<uint32_t>(OpParseU64(parts[23]));
        out->previous_login_time_sec = OpParseI64(parts[24]);
    }
    return true;
}

}  // namespace

bool SessionStore::LoadOperationResult(const std::string &operation_id, SessionOpStatus *status,
                                       std::string *op_kind, AcquireSessionResult *out) {
    if (!available_ || operation_id.empty() || !status)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> ignore;
    (void)ignore;
    // GET via Eval for simplicity (RedisClient has no Get)
    std::vector<std::string> keys{OpKey(operation_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    const char *get_lua = "return {redis.call('GET', KEYS[1]) or ''}";
    if (!lease->Eval(get_lua, keys, args, &reply) || reply.empty())
        return false;
    const std::string &v = reply[0];
    if (v.empty()) {
        *status = SessionOpStatus::NotFound;
        return true;
    }
    if (v == "PENDING") {
        *status = SessionOpStatus::Pending;
        return true;
    }
    AcquireSessionResult tmp;
    std::string kind;
    if (!UnpackOpResult(v, &kind, &tmp)) {
        *status = SessionOpStatus::NotFound;
        return false;
    }
    *status = SessionOpStatus::Done;
    if (op_kind)
        *op_kind = kind;
    if (out)
        *out = tmp;
    return true;
}

SessionStore::OpBegin SessionStore::BeginOperation(const std::string &operation_id,
                                                   AcquireSessionResult *cached,
                                                   std::string *op_kind_out, std::string *err) {
    if (operation_id.empty())
        return OpBegin::Execute;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        if (err)
            *err = "redis pool exhausted";
        return OpBegin::Error;
    }
    std::vector<std::string> keys{OpKey(operation_id)};
    std::vector<std::string> args{"120"};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaOpBegin, keys, args, &reply) || reply.empty()) {
        if (err)
            *err = "op begin lua failed";
        return OpBegin::Error;
    }
    if (reply[0] == "EXECUTE")
        return OpBegin::Execute;
    if (reply[0] == "DONE" && reply.size() > 1) {
        std::string kind;
        if (cached && UnpackOpResult(reply[1], &kind, cached)) {
            if (op_kind_out)
                *op_kind_out = kind;
            return OpBegin::Done;
        }
        if (err)
            *err = "bad cached op payload";
        return OpBegin::Error;
    }
    return OpBegin::Pending;
}

bool SessionStore::CompleteOperation(const std::string &operation_id, const std::string &op_kind,
                                     const AcquireSessionResult &result) {
    if (operation_id.empty() || !result.ok)
        return AbortOperation(operation_id);
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{OpKey(operation_id)};
    std::vector<std::string> args{PackOpResult(op_kind, result), "86400"};
    std::vector<std::string> reply;
    return lease->Eval(kLuaOpComplete, keys, args, &reply) && !reply.empty() && reply[0] == "1";
}

bool SessionStore::AbortOperation(const std::string &operation_id) {
    if (operation_id.empty())
        return true;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{OpKey(operation_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    return lease->Eval(kLuaOpAbort, keys, args, &reply);
}

bool SessionStore::GetSessionOperation(const std::string &operation_id, SessionOpStatus *status,
                                       std::string *op_kind, AcquireSessionResult *out) {
    return LoadOperationResult(operation_id, status, op_kind, out);
}

bool SessionStore::LoadSession(uint64_t player_id, SessionRecord *out) {
    if (!out || !available_)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> fields;
    if (!lease->HGetAll(SessionKey(player_id), &fields) || fields.empty())
        return false;
    out->token = fields["token"];
    out->session_id = fields["sessionId"];
    out->device_id = fields["deviceId"];
    out->server_id = static_cast<uint32_t>(std::strtoul(fields["serverId"].c_str(), nullptr, 10));
    out->login_time_sec = ParseI64(fields["loginTime"]);
    out->state = StateFromString(fields["state"]);
    out->gateway_id = fields["gatewayId"];
    out->connection_id = ParseU64(fields["connectionId"]);
    out->generation = ParseU64(fields["generation"]);
    out->disconnect_deadline_sec = ParseI64(fields["disconnectDeadline"]);
    out->gamelogic_instance_id = fields["gamelogicInstanceId"];
    out->map_instance_id = ParseU64(fields["mapInstanceId"]);
    out->map_owner_epoch = ParseU64(fields["mapOwnerEpoch"]);
    out->route_version = ParseU64(fields["routeVersion"]);
    if (out->token.empty())
        return false;
    if (fields["state"].empty())
        out->state = SessionState::Online;
    if (out->session_id.empty())
        out->session_id = out->token.substr(0, 16);
    return true;
}

bool SessionStore::ExpireIfGraceElapsed(uint64_t player_id, SessionRecord *rec) {
    if (!rec || rec->state != SessionState::Disconnected)
        return false;
    if (rec->disconnect_deadline_sec > 0 && NowUnixSec() <= rec->disconnect_deadline_sec)
        return false;
    if (grace_after_load_hook_)
        grace_after_load_hook_();
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        LOG_WARN << "SessionStore: grace CAS redis unavailable player_id=" << player_id;
        return false;
    }
    const char *lua = R"LUA(
local key = KEYS[1]
local sid = ARGV[1]
local fence = ARGV[2]
local gen = ARGV[3]
local deadline = tonumber(ARGV[4]) or 0
local now = tonumber(ARGV[5]) or 0
local raw = redis.call('HGETALL', key)
if #raw == 0 then return {'NOT_FOUND'} end
local f = {}
for i = 1, #raw, 2 do f[raw[i]] = raw[i + 1] end
if (f['state'] or '') ~= 'DISCONNECTED' then return {'STALE_SNAPSHOT'} end
if (f['sessionId'] or '') ~= sid then return {'STALE_SNAPSHOT'} end
if (f['token'] or '') ~= fence then return {'STALE_SNAPSHOT'} end
if (f['generation'] or '0') ~= gen then return {'STALE_SNAPSHOT'} end
local dl = tonumber(f['disconnectDeadline'] or '0') or 0
if dl > 0 and now <= dl then return {'NOT_DUE'} end
if deadline > 0 and now <= deadline then return {'NOT_DUE'} end
redis.call('DEL', key)
return {'EXPIRED_AND_DELETED'}
)LUA";
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{rec->session_id, rec->token, std::to_string(rec->generation),
                                  std::to_string(rec->disconnect_deadline_sec),
                                  std::to_string(NowUnixSec())};
    std::vector<std::string> reply;
    if (!lease->Eval(lua, keys, args, &reply) || reply.empty()) {
        LOG_WARN << "SessionStore: grace CAS lua failed player_id=" << player_id;
        return false;
    }
    const std::string &code = reply[0];
    if (code == "EXPIRED_AND_DELETED") {
        rec->state = SessionState::Offline;
        PlacementStore::Instance().ReleaseByPlayer(player_id);
        LOG_INFO << "SessionStore: grace elapsed player_id=" << player_id << " -> OFFLINE";
        return true;
    }
    if (code == "STALE_SNAPSHOT") {
        SessionRecord fresh;
        if (LoadSession(player_id, &fresh))
            *rec = fresh;
        return false;
    }
    if (code == "NOT_FOUND") {
        rec->state = SessionState::Offline;
        return true;
    }
    return false;
}

bool SessionStore::ForceExpireGraceFromStaleLocalForTest(uint64_t player_id) {
    SessionRecord rec;
    if (!LoadSession(player_id, &rec))
        return false;
    rec.disconnect_deadline_sec = 1;
    return ExpireIfGraceElapsed(player_id, &rec);
}

void SessionStore::SetLogicInstanceIds(std::vector<std::string> ids, bool publish_snapshot) {
    {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        logic_instance_ids_ = std::move(ids);
    }
    if (publish_snapshot) {
        auto snap = std::make_shared<HealthyLogicSnapshot>();
        snap->source = HealthyLogicSnapshot::Source::kStatic;
        snap->instance_ids = LogicInstanceIds();
        snap->state = snap->instance_ids.empty() ? HealthyLogicSnapshot::State::kEmpty
                                                 : HealthyLogicSnapshot::State::kBootstrap;
        HealthyLogicSnapshotStore::Instance().Publish(snap);
    }
}

std::vector<std::string> SessionStore::LogicInstanceIds() const {
    std::lock_guard<std::mutex> lk(cfg_mu_);
    return logic_instance_ids_;
}

bool SessionStore::AcquireSessionUnlocked(const AcquireSessionInput &in, AcquireSessionResult *out) {
    if (!out)
        return false;
    *out = AcquireSessionResult{};
    if (!available_) {
        out->message = "redis session store unavailable";
        out->error_code = "REDIS_UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.device_id.empty()) {
        out->message = "player_id and device_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }

    std::string logic_id;
    {
        auto snap = HealthyLogicSnapshotStore::Instance().Current();
        std::vector<std::string> ids;
        if (snap && snap->version > 0 &&
            (snap->state == HealthyLogicSnapshot::State::kActive ||
             snap->state == HealthyLogicSnapshot::State::kEmpty ||
             snap->state == HealthyLogicSnapshot::State::kBootstrap)) {
            ids = snap->instance_ids;
        } else {
            std::lock_guard<std::mutex> lk(cfg_mu_);
            ids = logic_instance_ids_;
        }
        if (!in.preferred_gamelogic_instance_id.empty()) {
            bool preferred_ok = false;
            for (const auto &id : ids) {
                if (id == in.preferred_gamelogic_instance_id) {
                    preferred_ok = true;
                    break;
                }
            }
            if (preferred_ok) {
                logic_id = in.preferred_gamelogic_instance_id;
            } else if (!ids.empty()) {
                logic_id = ids[static_cast<size_t>(in.player_id % ids.size())];
            }
        } else if (!ids.empty()) {
            logic_id = ids[static_cast<size_t>(in.player_id % ids.size())];
        }
    }
    if (logic_id.empty()) {
        out->message = "no healthy gamelogic";
        out->error_code = "NO_HEALTHY_GAMELOGIC";
        return false;
    }

    const std::string token = GenHex(32);
    const std::string session_id = GenHex(16);
    if (token.empty() || session_id.empty()) {
        out->message = "secure random failed";
        out->error_code = "RNG_FAILED";
        return false;
    }
    const int64_t login_time = NowUnixSec();
    int ttl = static_cast<int>(in.ttl_sec);
    if (ttl <= 0)
        ttl = default_ttl_sec_;
    if (ttl > long_ttl_sec_)
        ttl = long_ttl_sec_;

    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }

    std::vector<std::string> keys{SessionKey(in.player_id)};
    std::vector<std::string> args{
        in.kick_other_device ? "1" : "0",
        in.device_id,
        token,
        session_id,
        std::to_string(in.server_id),
        std::to_string(login_time),
        in.gateway_instance_id,
        logic_id,
        "0",
        "0",
        "1",
        std::to_string(ttl),
        std::to_string(NowUnixSec()),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaAcquire, keys, args, &reply) || reply.size() < 3) {
        out->message = "redis acquire lua failed";
        out->error_code = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : "acquire rejected";
        out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
        return false;
    }

    out->ok = true;
    out->message = "login ok";
    out->generation = ParseU64(reply[2]);
    out->route_version = reply.size() > 3 ? ParseU64(reply[3]) : 1;
    out->kicked_previous = reply.size() > 4 && reply[4] == "1";
    out->fence_token = reply.size() > 5 ? reply[5] : token;
    out->session_id = reply.size() > 6 ? reply[6] : session_id;
    out->gamelogic_instance_id = reply.size() > 7 ? reply[7] : logic_id;
    out->map_instance_id = reply.size() > 8 ? ParseU64(reply[8]) : 0;
    out->map_owner_epoch = reply.size() > 9 ? ParseU64(reply[9]) : 0;
    out->login_time_sec = login_time;
    out->server_id = in.server_id;
    if (out->kicked_previous && reply.size() > 13) {
        out->previous_gateway_instance_id = reply.size() > 10 ? reply[10] : "";
        out->previous_session_id = reply.size() > 11 ? reply[11] : "";
        out->previous_generation = reply.size() > 12 ? ParseU64(reply[12]) : 0;
        out->previous_fence_token = reply.size() > 13 ? reply[13] : "";
        out->previous_device_id = reply.size() > 14 ? reply[14] : "";
        out->previous_gamelogic_instance_id = reply.size() > 15 ? reply[15] : "";
        out->previous_map_instance_id = reply.size() > 16 ? ParseU64(reply[16]) : 0;
        out->previous_map_owner_epoch = reply.size() > 17 ? ParseU64(reply[17]) : 0;
        out->previous_route_version = reply.size() > 18 ? ParseU64(reply[18]) : 0;
        out->previous_server_id =
            reply.size() > 19 ? static_cast<uint32_t>(ParseU64(reply[19])) : 0;
        out->previous_login_time_sec = reply.size() > 20 ? static_cast<int64_t>(ParseU64(reply[20])) : 0;
    }
    LOG_INFO << "SessionStore: AcquireSession player_id=" << in.player_id
             << " generation=" << out->generation << " logic=" << out->gamelogic_instance_id
             << " kicked=" << (out->kicked_previous ? 1 : 0)
             << " prev_gw=" << out->previous_gateway_instance_id;
    return true;
}

bool SessionStore::AcquireSession(const AcquireSessionInput &in, AcquireSessionResult *out) {
    if (!out)
        return false;
    *out = AcquireSessionResult{};
    if (in.operation_id.empty())
        return AcquireSessionUnlocked(in, out);

    for (int attempt = 0; attempt < 50; ++attempt) {
        std::string kind;
        std::string err;
        AcquireSessionResult cached;
        const OpBegin st = BeginOperation(in.operation_id, &cached, &kind, &err);
        if (st == OpBegin::Done) {
            *out = cached;
            return out->ok;
        }
        if (st == OpBegin::Error) {
            out->message = err.empty() ? "op begin failed" : err;
            out->error_code = "OP_BEGIN_FAILED";
            return false;
        }
        if (st == OpBegin::Pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const bool ok = AcquireSessionUnlocked(in, out);
        if (ok && out->ok)
            CompleteOperation(in.operation_id, "acquire", *out);
        else
            AbortOperation(in.operation_id);
        return ok && out->ok;
    }
    out->message = "operation still pending";
    out->error_code = "OP_IN_PROGRESS";
    return false;
}

bool SessionStore::Login(const game::LoginReq &req, game::LoginRsp *rsp) {
    if (!rsp)
        return false;
    rsp->Clear();
    AcquireSessionInput in;
    in.account_id = req.player_id();
    in.player_id = req.player_id();
    in.device_id = req.device_id();
    in.server_id = req.server_id();
    in.ttl_sec = req.ttl_sec();
    in.kick_other_device = req.kick_other_device();
    AcquireSessionResult out;
    if (!AcquireSession(in, &out) || !out.ok) {
        rsp->set_ok(false);
        rsp->set_message(out.message.empty() ? "acquire failed" : out.message);
        return false;
    }
    rsp->set_ok(true);
    rsp->set_message(out.message);
    rsp->set_token(out.fence_token);
    rsp->set_server_id(out.server_id);
    rsp->set_login_time_sec(out.login_time_sec);
    rsp->set_kicked_previous(out.kicked_previous);
    rsp->set_session_id(out.session_id);
    rsp->set_generation(out.generation);
    return true;
}

bool SessionStore::Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp) {
    return Reconnect(req, rsp, nullptr);
}

bool SessionStore::Reconnect(const game::ReconnectReq &req, game::ReconnectRsp *rsp,
                             SessionRecord *route_out) {
    ReconnectSessionInput in;
    in.player_id = req.player_id();
    in.session_id = req.session_id();
    in.reconnect_ticket = req.reconnect_ticket();
    in.last_server_seq = req.last_server_seq();
    AcquireSessionResult out;
    SessionRecord route;
    const bool ok = ReconnectSession(in, &out, route_out ? &route : nullptr);
    if (!rsp)
        return ok;
    rsp->Clear();
    rsp->set_ok(ok && out.ok);
    rsp->set_message(out.message);
    rsp->set_token(out.fence_token);
    rsp->set_session_id(out.session_id);
    rsp->set_generation(out.generation);
    if (route_out && ok && out.ok)
        *route_out = route;
    return ok && out.ok;
}

bool SessionStore::ReconnectSession(const ReconnectSessionInput &in, AcquireSessionResult *out,
                                    SessionRecord *route_out) {
    if (!out)
        return false;
    *out = AcquireSessionResult{};
    if (route_out)
        *route_out = SessionRecord{};
    if (!available_) {
        out->message = "redis unavailable";
        out->error_code = "REDIS_UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.session_id.empty() || in.reconnect_ticket.empty()) {
        out->message = "player_id/session_id/reconnect_ticket required";
        out->error_code = "INVALID_ARG";
        return false;
    }

    auto run_once = [&]() -> bool {
        const std::string new_token = GenHex(32);
        if (new_token.empty()) {
            out->message = "secure random failed";
            out->error_code = "RNG_FAILED";
            return false;
        }
        auto lease = RedisPool::Instance().Acquire();
        if (!lease) {
            out->message = "redis pool exhausted";
            out->error_code = "POOL_EXHAUSTED";
            return false;
        }
        std::vector<std::string> keys{SessionKey(in.player_id)};
        std::vector<std::string> args{in.session_id, in.reconnect_ticket, new_token,
                                      std::to_string(default_ttl_sec_),
                                      std::to_string(NowUnixSec())};
        std::vector<std::string> reply;
        if (!lease->Eval(kLuaReconnect, keys, args, &reply) || reply.size() < 3) {
            out->message = "redis reconnect lua failed";
            out->error_code = "LUA_FAILED";
            return false;
        }
        if (reply[0] != "1") {
            out->message = reply.size() > 2 ? reply[2] : "reconnect rejected";
            out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
            return false;
        }
        out->ok = true;
        out->message = "reconnect ok";
        out->fence_token = reply.size() > 2 ? reply[2] : new_token;
        out->session_id = reply.size() > 3 ? reply[3] : in.session_id;
        out->generation = reply.size() > 4 ? ParseU64(reply[4]) : 0;
        out->gamelogic_instance_id = reply.size() > 5 ? reply[5] : "";
        out->map_instance_id = reply.size() > 6 ? ParseU64(reply[6]) : 0;
        out->map_owner_epoch = reply.size() > 7 ? ParseU64(reply[7]) : 0;
        out->route_version = reply.size() > 8 ? ParseU64(reply[8]) : 0;
        if (route_out) {
            route_out->token = out->fence_token;
            route_out->session_id = out->session_id;
            route_out->generation = out->generation;
            route_out->state = SessionState::Online;
            route_out->gamelogic_instance_id = out->gamelogic_instance_id;
            route_out->map_instance_id = out->map_instance_id;
            route_out->map_owner_epoch = out->map_owner_epoch;
            route_out->route_version = out->route_version;
        }
        LOG_INFO << "SessionStore: Reconnect player_id=" << in.player_id
                 << " generation=" << out->generation << " logic=" << out->gamelogic_instance_id;
        return true;
    };

    if (in.operation_id.empty())
        return run_once() && out->ok;

    for (int attempt = 0; attempt < 50; ++attempt) {
        std::string kind;
        std::string err;
        AcquireSessionResult cached;
        const OpBegin st = BeginOperation(in.operation_id, &cached, &kind, &err);
        if (st == OpBegin::Done) {
            *out = cached;
            if (route_out && out->ok) {
                route_out->token = out->fence_token;
                route_out->session_id = out->session_id;
                route_out->generation = out->generation;
                route_out->state = SessionState::Online;
                route_out->gamelogic_instance_id = out->gamelogic_instance_id;
                route_out->map_instance_id = out->map_instance_id;
                route_out->map_owner_epoch = out->map_owner_epoch;
                route_out->route_version = out->route_version;
            }
            return out->ok;
        }
        if (st == OpBegin::Error) {
            out->message = err.empty() ? "op begin failed" : err;
            out->error_code = "OP_BEGIN_FAILED";
            return false;
        }
        if (st == OpBegin::Pending) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const bool ok = run_once();
        if (ok && out->ok)
            CompleteOperation(in.operation_id, "reconnect", *out);
        else
            AbortOperation(in.operation_id);
        return ok && out->ok;
    }
    out->message = "operation still pending";
    out->error_code = "OP_IN_PROGRESS";
    return false;
}

bool SessionStore::PrepareReconnect(const ReconnectSessionInput &in, AcquireSessionResult *out) {
    if (!out)
        return false;
    *out = AcquireSessionResult{};
    if (!available_) {
        out->message = "redis unavailable";
        out->error_code = "REDIS_UNAVAILABLE";
        return false;
    }
    if (in.player_id == 0 || in.session_id.empty() || in.reconnect_ticket.empty() ||
        in.operation_id.empty()) {
        out->message = "player_id/session_id/ticket/operation_id required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    const std::string cand = GenHex(32);
    if (cand.empty()) {
        out->message = "secure random failed";
        out->error_code = "RNG_FAILED";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    std::vector<std::string> keys{SessionKey(in.player_id), ReconnectPendingKey(in.operation_id)};
    std::vector<std::string> args{in.session_id, in.reconnect_ticket, cand, "15",
                                  std::to_string(NowUnixSec()), in.operation_id};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaPrepareReconnect, keys, args, &reply) || reply.size() < 3) {
        out->message = "redis prepare lua failed";
        out->error_code = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : "prepare rejected";
        out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
        return false;
    }
    out->ok = true;
    out->message = "prepare ok";
    out->fence_token = reply.size() > 2 ? reply[2] : cand;
    out->generation = reply.size() > 3 ? ParseU64(reply[3]) : 0;
    out->session_id = reply.size() > 4 ? reply[4] : in.session_id;
    out->gamelogic_instance_id = reply.size() > 5 ? reply[5] : "";
    out->map_instance_id = reply.size() > 6 ? ParseU64(reply[6]) : 0;
    out->map_owner_epoch = reply.size() > 7 ? ParseU64(reply[7]) : 0;
    out->route_version = reply.size() > 8 ? ParseU64(reply[8]) : 0;
    return true;
}

bool SessionStore::CommitReconnect(uint64_t player_id, const std::string &operation_id,
                                   const std::string &candidate_fence, uint64_t candidate_generation,
                                   const std::string &gateway_instance_id, uint64_t connection_id,
                                   AcquireSessionResult *out) {
    if (!out)
        return false;
    *out = AcquireSessionResult{};
    if (!available_) {
        out->message = "redis unavailable";
        out->error_code = "REDIS_UNAVAILABLE";
        return false;
    }
    if (player_id == 0 || operation_id.empty() || candidate_fence.empty()) {
        out->message = "commit args required";
        out->error_code = "INVALID_ARG";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "redis pool exhausted";
        out->error_code = "POOL_EXHAUSTED";
        return false;
    }
    SessionRecord rec;
    const std::string sid = LoadSession(player_id, &rec) ? rec.session_id : std::string();
    std::vector<std::string> keys{SessionKey(player_id), ReconnectPendingKey(operation_id)};
    std::vector<std::string> args{sid, operation_id, candidate_fence,
                                  std::to_string(candidate_generation),
                                  std::to_string(default_ttl_sec_), gateway_instance_id,
                                  std::to_string(connection_id)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaCommitReconnect, keys, args, &reply) || reply.size() < 3) {
        out->message = "redis commit lua failed";
        out->error_code = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : "commit rejected";
        out->error_code = reply.size() > 1 ? reply[1] : "REJECTED";
        return false;
    }
    out->ok = true;
    out->message = "commit ok";
    out->fence_token = reply.size() > 2 ? reply[2] : candidate_fence;
    out->session_id = reply.size() > 3 ? reply[3] : sid;
    out->generation = reply.size() > 4 ? ParseU64(reply[4]) : candidate_generation;
    out->gamelogic_instance_id = reply.size() > 5 ? reply[5] : "";
    out->map_instance_id = reply.size() > 6 ? ParseU64(reply[6]) : 0;
    out->map_owner_epoch = reply.size() > 7 ? ParseU64(reply[7]) : 0;
    out->route_version = reply.size() > 8 ? ParseU64(reply[8]) : 0;
    return true;
}

bool SessionStore::AbortReconnect(uint64_t player_id, const std::string &operation_id,
                                  const std::string &candidate_fence) {
    (void)player_id;
    if (!available_ || operation_id.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{ReconnectPendingKey(operation_id)};
    std::vector<std::string> args{operation_id, candidate_fence};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaAbortReconnect, keys, args, &reply) || reply.empty())
        return false;
    return reply[0] == "1";
}

bool SessionStore::BindConnection(uint64_t player_id, const std::string &token,
                                  const std::string &gateway_id, uint64_t connection_id) {
    if (!available_ || player_id == 0 || token.empty())
        return false;
    {
        auto lease = RedisPool::Instance().Acquire();
        if (!lease)
            return false;
        std::vector<std::string> keys{SessionKey(player_id)};
        std::vector<std::string> args{token, gateway_id, std::to_string(connection_id),
                                      std::to_string(default_ttl_sec_)};
        std::vector<std::string> reply;
        if (!lease->Eval(kLuaBindConnection, keys, args, &reply) || reply.empty())
            return false;
        if (reply[0] != "1")
            return false;
    }
    TrackOnline(player_id);
    return true;
}

bool SessionStore::MarkDisconnected(uint64_t player_id, const std::string &token,
                                    uint64_t generation) {
    if (!available_ || player_id == 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{token, std::to_string(generation), std::to_string(grace_sec_),
                                  std::to_string(NowUnixSec()), std::to_string(default_ttl_sec_)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaMarkDisconnected, keys, args, &reply) || reply.size() < 2)
        return false;
    if (reply[0] != "1") {
        LOG_INFO << "SessionStore: MarkDisconnected ignored player_id=" << player_id
                 << " reason=" << reply[1];
        return false;
    }
    LOG_INFO << "SessionStore: DISCONNECTED player_id=" << player_id << " grace_sec=" << grace_sec_;
    lease = RedisPool::Lease();
    UntrackOnline(player_id);
    return true;
}

bool SessionStore::Kick(uint64_t player_id, const std::string &reason, std::string *err) {
    KickResult r;
    const bool ok = Kick(player_id, reason, &r);
    if (err && !ok)
        *err = r.message.empty() ? r.error_code : r.message;
    return ok;
}

bool SessionStore::Kick(uint64_t player_id, const std::string &reason, KickResult *out) {
    KickResult local;
    KickResult *r = out ? out : &local;
    *r = KickResult{};
    if (!available_ || player_id == 0) {
        r->error_code = "unavailable";
        r->message = "unavailable";
        return false;
    }
    const std::string neu = GenHex(32);
    if (neu.empty()) {
        r->error_code = "RNG_FAILED";
        r->message = "RNG_FAILED";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        r->error_code = "POOL_EXHAUSTED";
        r->message = "POOL_EXHAUSTED";
        return false;
    }
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{neu, std::to_string(default_ttl_sec_)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaKick, keys, args, &reply) || reply.size() < 2) {
        r->error_code = "LUA_FAILED";
        r->message = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        r->error_code = reply.size() > 1 ? reply[1] : "KICK_FAILED";
        r->message = reply.size() > 2 ? reply[2] : r->error_code;
        return false;
    }
    r->ok = true;
    r->message = "kicked";
    if (reply.size() > 2)
        r->new_generation = static_cast<uint64_t>(std::strtoull(reply[2].c_str(), nullptr, 10));
    if (reply.size() > 3)
        r->old_gateway_id = reply[3];
    if (reply.size() > 4)
        r->old_token = reply[4];
    if (reply.size() > 5)
        r->old_session_id = reply[5];
    if (reply.size() > 6)
        r->old_generation = static_cast<uint64_t>(std::strtoull(reply[6].c_str(), nullptr, 10));
    else if (r->new_generation > 0)
        r->old_generation = r->new_generation - 1;
    LOG_INFO << "SessionStore: Kick player_id=" << player_id << " reason=" << reason
             << " generation=" << r->new_generation << " old_gw=" << r->old_gateway_id;
    lease = RedisPool::Lease();
    UntrackOnline(player_id);
    return true;
}

bool SessionStore::InvalidateOperation(const std::string &operation_id) {
    if (!available_ || operation_id.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{OpKey(operation_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    return lease->Eval(kLuaOpInvalidate, keys, args, &reply) && !reply.empty() && reply[0] == "1";
}

bool SessionStore::RestorePreviousSession(uint64_t player_id, const std::string &current_fence,
                                          const AcquireSessionResult &previous,
                                          const std::string &operation_id, std::string *err) {
    if (!available_ || player_id == 0 || current_fence.empty()) {
        if (err)
            *err = "invalid restore args";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        if (err)
            *err = "POOL_EXHAUSTED";
        return false;
    }
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{
        current_fence,
        previous.previous_fence_token,
        previous.previous_session_id,
        std::to_string(previous.previous_generation),
        previous.previous_gateway_instance_id,
        previous.previous_device_id,
        previous.previous_gamelogic_instance_id,
        std::to_string(previous.previous_map_instance_id),
        std::to_string(previous.previous_map_owner_epoch),
        std::to_string(previous.previous_route_version),
        std::to_string(previous.previous_server_id),
        std::to_string(previous.previous_login_time_sec),
        std::to_string(default_ttl_sec_),
    };
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaRestorePrevious, keys, args, &reply) || reply.empty()) {
        if (err)
            *err = "LUA_FAILED";
        return false;
    }
    if (reply[0] != "1") {
        if (err)
            *err = reply.size() > 1 ? reply[1] : "RESTORE_FAILED";
        return false;
    }
    if (!operation_id.empty())
        InvalidateOperation(operation_id);
    LOG_INFO << "SessionStore: RestorePrevious player_id=" << player_id
             << " result=" << (reply.size() > 1 ? reply[1] : "ok")
             << " prev_gw=" << previous.previous_gateway_instance_id;
    return true;
}

bool SessionStore::Validate(const game::ValidateSessionReq &req, game::ValidateSessionRsp *rsp) {
    if (!rsp)
        return false;
    rsp->Clear();
    if (!available_) {
        rsp->set_ok(false);
        rsp->set_message("redis unavailable");
        return false;
    }
    SessionRecord rec;
    bool online = LoadSession(req.player_id(), &rec);
    if (online && ExpireIfGraceElapsed(req.player_id(), &rec))
        online = false;
    rsp->set_online(online && rec.state == SessionState::Online);
    if (!online) {
        rsp->set_ok(true);
        rsp->set_valid(false);
        rsp->set_message("not online");
        return true;
    }
    const bool valid =
        !req.token().empty() && req.token() == rec.token && rec.state == SessionState::Online;
    rsp->set_ok(true);
    rsp->set_valid(valid);
    rsp->set_server_id(rec.server_id);
    rsp->set_device_id(rec.device_id);
    rsp->set_login_time_sec(rec.login_time_sec);
    rsp->set_message(valid ? "token ok" : "token mismatch or not ONLINE");
    return true;
}

bool SessionStore::CheckOnline(const game::CheckOnlineReq &req, game::CheckOnlineRsp *rsp) {
    if (!rsp)
        return false;
    rsp->Clear();
    if (!available_) {
        rsp->set_ok(false);
        rsp->set_message("redis unavailable");
        return false;
    }
    const bool online = IsPlayerOnline(req.player_id());
    rsp->set_ok(true);
    rsp->set_online(online);
    rsp->set_message(online ? "online" : "offline");
    return true;
}

bool SessionStore::IsPlayerOnline(uint64_t player_id) {
    if (!available_ || player_id == 0)
        return false;
    SessionRecord rec;
    if (!LoadSession(player_id, &rec))
        return false;
    if (ExpireIfGraceElapsed(player_id, &rec))
        return false;
    return rec.state == SessionState::Online;
}

bool SessionStore::Logout(const game::LogoutReq &req, game::LogoutRsp *rsp) {
    if (!rsp)
        return false;
    rsp->Clear();
    if (!available_) {
        rsp->set_ok(false);
        rsp->set_message("redis unavailable");
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        rsp->set_ok(false);
        rsp->set_message("redis pool exhausted");
        return false;
    }
    std::vector<std::string> keys{SessionKey(req.player_id())};
    std::vector<std::string> args{req.token()};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaLogout, keys, args, &reply) || reply.empty()) {
        rsp->set_ok(false);
        rsp->set_message("redis logout lua failed");
        return false;
    }
    if (reply[0] != "1") {
        rsp->set_ok(false);
        rsp->set_message(reply.size() > 2 ? reply[2] : "logout rejected");
        return false;
    }
    rsp->set_ok(true);
    rsp->set_message(reply.size() > 1 && reply[1] == "ALREADY_OFFLINE" ? "already offline"
                                                                        : "logout ok");
    lease = RedisPool::Lease();
    PlacementStore::Instance().ReleaseByPlayer(req.player_id());
    UntrackOnline(req.player_id());
    return true;
}

bool SessionStore::UpdatePlayerRoute(uint64_t player_id, const std::string &fence_token,
                                     const std::string &gamelogic_instance_id,
                                     uint64_t map_instance_id, uint64_t map_owner_epoch,
                                     uint64_t route_version, const std::string &gateway_instance_id,
                                     const std::string &push_endpoint, uint64_t *route_version_out,
                                     std::string *err) {
    if (!available_ || player_id == 0 || fence_token.empty()) {
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
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{fence_token,
                                  gamelogic_instance_id,
                                  std::to_string(map_instance_id),
                                  std::to_string(map_owner_epoch),
                                  std::to_string(route_version),
                                  gateway_instance_id,
                                  push_endpoint};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaUpdatePlayerRoute, keys, args, &reply) || reply.size() < 2) {
        if (err)
            *err = "lua failed";
        return false;
    }
    if (reply[0] != "1") {
        if (err)
            *err = reply.size() > 2 ? reply[2] : reply[1];
        return false;
    }
    if (route_version_out && reply.size() > 2)
        *route_version_out = ParseU64(reply[2]);
    return true;
}

bool SessionStore::BeginPlayerTransfer(const TransferBeginIn &in, TransferBeginOut *out) {
    if (!out)
        return false;
    *out = TransferBeginOut{};
    if (!available_ || in.player_id == 0 || in.fence_token.empty() || in.to_logic.empty()) {
        out->message = "invalid arg";
        out->error_code = "INVALID_ARG";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "pool exhausted";
        out->error_code = "POOL";
        return false;
    }
    std::vector<std::string> keys{SessionKey(in.player_id)};
    std::vector<std::string> args{in.fence_token,
                                  std::to_string(in.expected_route_version),
                                  in.from_logic,
                                  in.to_logic,
                                  std::to_string(in.map_instance_id),
                                  std::to_string(in.map_owner_epoch),
                                  in.transfer_id,
                                  in.gateway_instance_id};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaBeginPlayerTransfer, keys, args, &reply) || reply.size() < 2) {
        out->message = "lua failed";
        out->error_code = "LUA";
        return false;
    }
    out->error_code = reply[1];
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : reply[1];
        return false;
    }
    out->ok = true;
    out->transfer_id = reply.size() > 2 ? reply[2] : in.transfer_id;
    out->route_version = reply.size() > 3 ? ParseU64(reply[3]) : 0;
    out->route_state = reply.size() > 4 ? reply[4] : "TRANSFERRING";
    out->message = "ok";
    return true;
}

bool SessionStore::CommitPlayerTransfer(const TransferCommitIn &in, TransferCommitOut *out) {
    if (!out)
        return false;
    *out = TransferCommitOut{};
    if (!available_ || in.player_id == 0 || in.fence_token.empty() || in.transfer_id.empty() ||
        in.to_logic.empty()) {
        out->message = "invalid arg";
        out->error_code = "INVALID_ARG";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        out->message = "pool exhausted";
        out->error_code = "POOL";
        return false;
    }
    std::vector<std::string> keys{SessionKey(in.player_id)};
    std::vector<std::string> args{in.fence_token,
                                  in.transfer_id,
                                  in.to_logic,
                                  std::to_string(in.map_instance_id),
                                  std::to_string(in.map_owner_epoch),
                                  in.gateway_instance_id};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaCommitPlayerTransfer, keys, args, &reply) || reply.size() < 2) {
        out->message = "lua failed";
        out->error_code = "LUA";
        return false;
    }
    out->error_code = reply[1];
    if (reply[0] != "1") {
        out->message = reply.size() > 2 ? reply[2] : reply[1];
        return false;
    }
    out->ok = true;
    out->route_version = reply.size() > 2 ? ParseU64(reply[2]) : 0;
    out->gamelogic_instance_id = reply.size() > 3 ? reply[3] : in.to_logic;
    out->map_instance_id = reply.size() > 4 ? ParseU64(reply[4]) : in.map_instance_id;
    out->map_owner_epoch = reply.size() > 5 ? ParseU64(reply[5]) : in.map_owner_epoch;
    out->route_state = reply.size() > 6 ? reply[6] : "ONLINE";
    out->message = "ok";
    return true;
}

bool SessionStore::AbortPlayerTransfer(uint64_t player_id, const std::string &fence_token,
                                       const std::string &transfer_id, std::string *err,
                                       uint64_t *route_version_out) {
    if (!available_ || player_id == 0 || fence_token.empty()) {
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
    std::vector<std::string> keys{SessionKey(player_id)};
    std::vector<std::string> args{fence_token, transfer_id};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaAbortPlayerTransfer, keys, args, &reply) || reply.size() < 2) {
        if (err)
            *err = "lua failed";
        return false;
    }
    if (reply[0] != "1") {
        if (err)
            *err = reply.size() > 2 ? reply[2] : reply[1];
        return false;
    }
    if (route_version_out && reply.size() > 2)
        *route_version_out = ParseU64(reply[2]);
    return true;
}

bool SessionStore::GetPlayerRoute(uint64_t player_id, const std::string &fence_token,
                                  SessionRecord *out, std::string *route_state,
                                  std::string *transfer_id, std::string *err) {
    if (!available_ || !out) {
        if (err)
            *err = "unavailable";
        return false;
    }
    if (!LoadSession(player_id, out)) {
        if (err)
            *err = "not found";
        return false;
    }
    if (!fence_token.empty() && out->token != fence_token) {
        if (err)
            *err = "fence mismatch";
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (lease) {
        std::map<std::string, std::string> fields;
        if (lease->HGetAll(SessionKey(player_id), &fields)) {
            if (route_state)
                *route_state = fields.count("routeState") ? fields["routeState"] : "ONLINE";
            if (transfer_id)
                *transfer_id = fields.count("transferId") ? fields["transferId"] : "";
        }
    } else if (route_state) {
        *route_state = "ONLINE";
    }
    return true;
}

bool SessionStore::ValidateToken(uint64_t player_id, const std::string &token, std::string *err) {
    if (!available_) {
        if (err)
            *err = "redis unavailable";
        return false;
    }
    if (token.empty()) {
        if (err)
            *err = "session_token required";
        return false;
    }
    SessionRecord rec;
    if (!LoadSession(player_id, &rec)) {
        if (err)
            *err = "not logged in";
        return false;
    }
    if (ExpireIfGraceElapsed(player_id, &rec)) {
        if (err)
            *err = "session expired";
        return false;
    }
    if (rec.state != SessionState::Online) {
        if (err)
            *err = "session not ONLINE (disconnected or closing)";
        return false;
    }
    if (rec.token != token) {
        if (err)
            *err = "invalid session token";
        return false;
    }
    return true;
}

std::string SessionStore::OnlineSetKey() const {
    return key_prefix_ + "online:players";
}

void SessionStore::TrackOnline(uint64_t player_id) {
    if (!available_ || player_id == 0)
        return;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return;
    (void)lease->SAdd(OnlineSetKey(), std::to_string(player_id));
}

void SessionStore::UntrackOnline(uint64_t player_id) {
    if (!available_ || player_id == 0)
        return;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return;
    (void)lease->SRem(OnlineSetKey(), std::to_string(player_id));
}

bool SessionStore::ConsumeKeyedQuota(const std::string &key, int limit, int window_sec) {
    if (!available_ || key.empty() || limit <= 0)
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    static const char *kLua =
        "local c=redis.call('INCR', KEYS[1]); if c==1 then redis.call('EXPIRE', KEYS[1], "
        "tonumber(ARGV[1])) end; return c";
    std::vector<std::string> out;
    if (!lease->Eval(kLua, {key}, {std::to_string(window_sec > 0 ? window_sec : 2)}, &out) ||
        out.empty())
        return false;
    return std::atoi(out[0].c_str()) <= limit;
}

bool SessionStore::ConsumeChatQuota(uint64_t player_id, int limit, int window_sec) {
    if (player_id == 0)
        return false;
    return ConsumeKeyedQuota(key_prefix_ + "chatrate:" + std::to_string(player_id), limit,
                             window_sec);
}

bool SessionStore::ConsumeNameQueryQuota(uint64_t player_id, int limit, int window_sec) {
    if (player_id == 0)
        return false;
    return ConsumeKeyedQuota(key_prefix_ + "namequery:" + std::to_string(player_id), limit,
                             window_sec);
}

uint64_t SessionStore::NextWorldChatMessageId() {
    if (!available_)
        return 0;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return 0;
    int64_t n = 0;
    if (!lease->Incr(key_prefix_ + "chat:world:seq", &n) || n <= 0)
        return 0;
    return static_cast<uint64_t>(n);
}

bool SessionStore::QueryPublicOnlineState(uint64_t player_id, std::string *state) {
    if (!state)
        return false;
    *state = "offline";
    if (!available_ || player_id == 0)
        return true;
    SessionRecord rec;
    if (!LoadSession(player_id, &rec))
        return true;
    if (ExpireIfGraceElapsed(player_id, &rec))
        return true;
    if (rec.state == SessionState::Online)
        *state = "online";
    else if (rec.state == SessionState::Disconnected)
        *state = "disconnected";
    else
        *state = "offline";
    return true;
}

bool SessionStore::ListOnlinePushTargets(std::vector<OnlinePushTarget> *out, size_t max_n) {
    if (!out)
        return false;
    out->clear();
    if (!available_)
        return false;
    std::vector<std::string> members;
    {
        auto lease = RedisPool::Instance().Acquire();
        if (!lease)
            return false;
        if (!lease->SMembers(OnlineSetKey(), &members))
            return false;
    }
    if (max_n == 0)
        max_n = 256;
    size_t n = 0;
    for (const auto &m : members) {
        if (n >= max_n)
            break;
        const uint64_t pid = static_cast<uint64_t>(std::strtoull(m.c_str(), nullptr, 10));
        if (pid == 0)
            continue;
        SessionRecord rec;
        if (!LoadSession(pid, &rec)) {
            UntrackOnline(pid);
            continue;
        }
        if (ExpireIfGraceElapsed(pid, &rec) || rec.state != SessionState::Online) {
            UntrackOnline(pid);
            continue;
        }
        if (rec.gateway_id.empty() || rec.session_id.empty())
            continue;
        OnlinePushTarget t;
        t.player_id = pid;
        t.gateway_id = rec.gateway_id;
        t.session_id = rec.session_id;
        t.fence_token = rec.token;
        t.generation = rec.generation;
        out->push_back(std::move(t));
        ++n;
    }
    return true;
}
