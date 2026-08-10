#include "PushReplayStore.h"

#include "Logging.h"
#include "PushReplayCache.h"
#include "RedisPool.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool UnpackEntry(const std::string &raw, PushReplayEntry *out) {
    if (!out || raw.empty())
        return false;
    const size_t p0 = raw.find('\t');
    if (p0 == std::string::npos)
        return false;
    const size_t p1 = raw.find('\t', p0 + 1);
    if (p1 == std::string::npos)
        return false;
    const uint64_t seq = std::strtoull(raw.substr(0, p0).c_str(), nullptr, 10);
    const size_t type_len =
        static_cast<size_t>(std::strtoul(raw.substr(p0 + 1, p1 - p0 - 1).c_str(), nullptr, 10));
    const size_t type_start = p1 + 1;
    if (type_start + type_len >= raw.size() || raw[type_start + type_len] != '\t')
        return false;
    const std::string type = raw.substr(type_start, type_len);
    const size_t plen_start = type_start + type_len + 1;
    const size_t p2 = raw.find('\t', plen_start);
    if (p2 == std::string::npos)
        return false;
    const size_t payload_len =
        static_cast<size_t>(std::strtoul(raw.substr(plen_start, p2 - plen_start).c_str(), nullptr, 10));
    const size_t payload_start = p2 + 1;
    if (payload_start + payload_len != raw.size())
        return false;
    out->server_seq = seq;
    out->message_type = type;
    out->payload = raw.substr(payload_start, payload_len);
    out->reliable = true;
    return true;
}

const char kLuaAppend[] = R"LUA(
local seq_key = KEYS[1]
local list_key = KEYS[2]
local typ = ARGV[1]
local payload = ARGV[2]
local cap = tonumber(ARGV[3])
local ttl = tonumber(ARGV[4])
local seq = redis.call('INCR', seq_key)
local entry = tostring(seq) .. '\t' .. tostring(string.len(typ)) .. '\t' .. typ .. '\t' .. tostring(string.len(payload)) .. '\t' .. payload
redis.call('RPUSH', list_key, entry)
redis.call('LTRIM', list_key, -cap, -1)
redis.call('EXPIRE', list_key, ttl)
redis.call('EXPIRE', seq_key, ttl)
return {tostring(seq)}
)LUA";

const char kLuaReplay[] = R"LUA(
local list_key = KEYS[1]
local seq_key = KEYS[2]
local last = tonumber(ARGV[1]) or 0
local entries = redis.call('LRANGE', list_key, 0, -1)
if #entries == 0 then
  local cur = tonumber(redis.call('GET', seq_key) or '0') or 0
  if last == 0 or last >= cur then return {'1', 'OK'} end
  return {'0', 'NEED_SNAPSHOT'}
end
local first_seq = tonumber(string.match(entries[1], '^(%d+)')) or 0
if last > 0 and first_seq > last + 1 then
  return {'0', 'NEED_SNAPSHOT'}
end
local out = {'1', 'OK'}
for _, v in ipairs(entries) do
  local seq = tonumber(string.match(v, '^(%d+)'))
  if seq and seq > last then
    out[#out + 1] = v
  end
end
return out
)LUA";

const char kLuaAck[] = R"LUA(
local list_key = KEYS[1]
local meta_key = KEYS[2]
local seq_key = KEYS[3]
local ack = tonumber(ARGV[1])
local ttl = tonumber(ARGV[2])
if not ack or ack <= 0 then
  return {'0', 'ERR_ACK_INVALID'}
end
local cur = tonumber(redis.call('GET', seq_key) or '0') or 0
local last = tonumber(redis.call('HGET', meta_key, 'lastAck') or '0') or 0
if ack > cur then
  return {'0', 'ERR_ACK_AHEAD', tostring(cur), tostring(last)}
end
if ack < last then
  return {'0', 'ERR_ACK_STALE', tostring(cur), tostring(last)}
end
if ack == last then
  redis.call('EXPIRE', meta_key, ttl)
  return {'1', 'DUP', tostring(ack)}
end
while true do
  local v = redis.call('LINDEX', list_key, 0)
  if not v then break end
  local seq = tonumber(string.match(v, '^(%d+)'))
  if not seq or seq > ack then break end
  redis.call('LPOP', list_key)
end
redis.call('HSET', meta_key, 'lastAck', tostring(ack))
redis.call('EXPIRE', meta_key, ttl)
redis.call('EXPIRE', list_key, ttl)
redis.call('EXPIRE', seq_key, ttl)
return {'1', 'OK', tostring(ack)}
)LUA";

const char kLuaGetSeq[] = R"LUA(
local v = redis.call('GET', KEYS[1])
if not v then return {'0'} end
return {tostring(v)}
)LUA";

}  // namespace

PushReplayStore &PushReplayStore::Instance() {
    static PushReplayStore g;
    return g;
}

bool PushReplayStore::InitFromSessionPrefix(const std::string &key_prefix, size_t cap,
                                            int ttl_sec) {
    if (!key_prefix.empty())
        key_prefix_ = key_prefix;
    if (cap > 0)
        cap_ = cap;
    if (ttl_sec > 0)
        ttl_sec_ = ttl_sec;
    available_ = RedisPool::Instance().ready();
    if (available_)
        LOG_INFO << "PushReplayStore ready prefix=" << key_prefix_ << " cap=" << cap_
                 << " ttl_sec=" << ttl_sec_;
    return available_;
}

std::string PushReplayStore::SeqKey(uint64_t player_id, const std::string &session_id) const {
    return key_prefix_ + "push:seq:" + std::to_string(player_id) + ":" + session_id;
}

std::string PushReplayStore::ListKey(uint64_t player_id, const std::string &session_id) const {
    return key_prefix_ + "push:replay:" + std::to_string(player_id) + ":" + session_id;
}

std::string PushReplayStore::MetaKey(uint64_t player_id, const std::string &session_id) const {
    return key_prefix_ + "push:meta:" + std::to_string(player_id) + ":" + session_id;
}

uint64_t PushReplayStore::AppendReliable(uint64_t player_id, const std::string &session_id,
                                         const std::string &message_type,
                                         const std::string &payload) {
    if (!available_ || player_id == 0 || session_id.empty())
        return 0;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return 0;
    std::vector<std::string> keys{SeqKey(player_id, session_id), ListKey(player_id, session_id)};
    std::vector<std::string> args{message_type, payload, std::to_string(cap_),
                                  std::to_string(ttl_sec_)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaAppend, keys, args, &reply) || reply.empty())
        return 0;
    return std::strtoull(reply[0].c_str(), nullptr, 10);
}

bool PushReplayStore::ReplayAfter(uint64_t player_id, const std::string &session_id,
                                  uint64_t last_acked_seq, std::vector<PushReplayEntry> *out,
                                  bool *need_snapshot) {
    if (!out)
        return false;
    out->clear();
    if (need_snapshot)
        *need_snapshot = false;
    if (!available_ || player_id == 0 || session_id.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> keys{ListKey(player_id, session_id), SeqKey(player_id, session_id)};
    std::vector<std::string> args{std::to_string(last_acked_seq)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaReplay, keys, args, &reply) || reply.size() < 2)
        return false;
    if (reply[0] != "1") {
        if (need_snapshot)
            *need_snapshot = (reply[1] == "NEED_SNAPSHOT");
        return false;
    }
    for (size_t i = 2; i < reply.size(); ++i) {
        PushReplayEntry e;
        if (UnpackEntry(reply[i], &e))
            out->push_back(std::move(e));
    }
    return true;
}

PushReplayStore::AckResult PushReplayStore::Ack(uint64_t player_id, const std::string &session_id,
                                                uint64_t ack_seq) {
    AckResult r;
    if (!available_ || player_id == 0 || session_id.empty() || ack_seq == 0) {
        r.status = AckStatus::Invalid;
        r.error_code = "ERR_ACK_INVALID";
        return r;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        r.status = AckStatus::Unavailable;
        r.error_code = "ERR_ACK_UNAVAILABLE";
        return r;
    }
    std::vector<std::string> keys{ListKey(player_id, session_id), MetaKey(player_id, session_id),
                                  SeqKey(player_id, session_id)};
    std::vector<std::string> args{std::to_string(ack_seq), std::to_string(ttl_sec_)};
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaAck, keys, args, &reply) || reply.size() < 2) {
        r.status = AckStatus::Unavailable;
        r.error_code = "ERR_ACK_UNAVAILABLE";
        return r;
    }
    if (reply[0] == "1") {
        if (reply[1] == "DUP") {
            r.status = AckStatus::Duplicate;
            r.error_code = "ERR_ACK_DUPLICATE";
        } else {
            r.status = AckStatus::Ok;
            r.error_code.clear();
        }
        r.trimmed_to_seq = ack_seq;
        return r;
    }
    r.error_code = reply[1];
    if (reply[1] == "ERR_ACK_AHEAD")
        r.status = AckStatus::Ahead;
    else if (reply[1] == "ERR_ACK_STALE")
        r.status = AckStatus::Stale;
    else
        r.status = AckStatus::Invalid;
    return r;
}

uint64_t PushReplayStore::CurrentSeq(uint64_t player_id, const std::string &session_id) {
    if (!available_ || player_id == 0 || session_id.empty())
        return 0;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return 0;
    std::vector<std::string> keys{SeqKey(player_id, session_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    if (!lease->Eval(kLuaGetSeq, keys, args, &reply) || reply.empty())
        return 0;
    return std::strtoull(reply[0].c_str(), nullptr, 10);
}

bool PushReplayStore::InvalidateSession(uint64_t player_id, const std::string &session_id) {
    if (!available_ || player_id == 0 || session_id.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    const char *lua = R"LUA(
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3])
return {'1'}
)LUA";
    std::vector<std::string> keys{SeqKey(player_id, session_id), ListKey(player_id, session_id),
                                  MetaKey(player_id, session_id)};
    std::vector<std::string> args;
    std::vector<std::string> reply;
    return lease->Eval(lua, keys, args, &reply);
}
