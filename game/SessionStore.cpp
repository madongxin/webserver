#include "SessionStore.h"

#include "Logging.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace {

int64_t NowUnixSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string GenHex(size_t n) {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char hex[] = "0123456789abcdef";
    std::string s(n, '0');
    for (char &c : s)
        c = hex[gen() & 0xf];
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
                 int *ttl, int *long_ttl, int *grace) {
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
    }
    return true;
}

RedisClient &Client() {
    static RedisClient c;
    return c;
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
    if (!ParseConfig(path, &host, &port, &password, &ttl, &long_ttl, &grace)) {
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
    if (!Client().Connect(host, port, password)) {
        LOG_ERROR << "SessionStore: Redis connect failed " << host << ":" << port;
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "SessionStore: Redis ok " << host << ":" << port
             << " default_ttl_sec=" << default_ttl_sec_ << " grace_sec=" << grace_sec_;
    return true;
}

std::string SessionStore::SessionKey(uint64_t player_id) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "game:session:%llu",
                  static_cast<unsigned long long>(player_id));
    return buf;
}

bool SessionStore::LoadSession(uint64_t player_id, SessionRecord *out) {
    if (!out || !available_)
        return false;
    std::map<std::string, std::string> fields;
    if (!Client().HGetAll(SessionKey(player_id), &fields) || fields.empty())
        return false;
    out->token = fields["token"];
    out->session_id = fields["sessionId"];
    out->device_id = fields["deviceId"];
    out->server_id = static_cast<uint32_t>(std::strtoul(fields["serverId"].c_str(), nullptr, 10));
    out->login_time_sec =
        static_cast<int64_t>(std::strtoll(fields["loginTime"].c_str(), nullptr, 10));
    out->state = StateFromString(fields["state"]);
    out->gateway_id = fields["gatewayId"];
    out->connection_id = static_cast<int>(std::atoi(fields["connectionId"].c_str()));
    out->generation =
        static_cast<uint64_t>(std::strtoull(fields["generation"].c_str(), nullptr, 10));
    out->disconnect_deadline_sec =
        static_cast<int64_t>(std::strtoll(fields["disconnectDeadline"].c_str(), nullptr, 10));
    out->gamelogic_instance_id = fields["gamelogicInstanceId"];
    out->map_instance_id =
        static_cast<uint64_t>(std::strtoull(fields["mapInstanceId"].c_str(), nullptr, 10));
    out->map_owner_epoch =
        static_cast<uint64_t>(std::strtoull(fields["mapOwnerEpoch"].c_str(), nullptr, 10));
    out->route_version =
        static_cast<uint64_t>(std::strtoull(fields["routeVersion"].c_str(), nullptr, 10));
    if (out->token.empty())
        return false;
    // 兼容旧数据：无 state 视为 ONLINE
    if (fields["state"].empty())
        out->state = SessionState::Online;
    if (out->session_id.empty())
        out->session_id = out->token.substr(0, 16);
    return true;
}

bool SessionStore::SaveSession(uint64_t player_id, const SessionRecord &rec, int ttl_sec) {
    if (!available_ || ttl_sec <= 0)
        return false;
    const std::string key = SessionKey(player_id);
    std::map<std::string, std::string> fields;
    fields["token"] = rec.token;
    fields["sessionId"] = rec.session_id;
    fields["serverId"] = std::to_string(rec.server_id);
    fields["loginTime"] = std::to_string(rec.login_time_sec);
    fields["deviceId"] = rec.device_id;
    fields["state"] = StateToString(rec.state);
    fields["gatewayId"] = rec.gateway_id;
    fields["connectionId"] = std::to_string(rec.connection_id);
    fields["generation"] = std::to_string(rec.generation);
    fields["disconnectDeadline"] = std::to_string(rec.disconnect_deadline_sec);
    fields["gamelogicInstanceId"] = rec.gamelogic_instance_id;
    fields["mapInstanceId"] = std::to_string(rec.map_instance_id);
    fields["mapOwnerEpoch"] = std::to_string(rec.map_owner_epoch);
    fields["routeVersion"] = std::to_string(rec.route_version);
    if (!Client().HSet(key, fields))
        return false;
    return Client().Expire(key, ttl_sec);
}

bool SessionStore::ExpireIfGraceElapsed(uint64_t player_id, SessionRecord *rec) {
    if (!rec || rec->state != SessionState::Disconnected)
        return false;
    if (rec->disconnect_deadline_sec > 0 && NowUnixSec() <= rec->disconnect_deadline_sec)
        return false;
    Client().Del(SessionKey(player_id));
    rec->state = SessionState::Offline;
    LOG_INFO << "SessionStore: grace elapsed player_id=" << player_id << " -> OFFLINE";
    return true;
}

void SessionStore::SetLogicInstanceIds(std::vector<std::string> ids) {
    if (!ids.empty())
        logic_instance_ids_ = std::move(ids);
}

bool SessionStore::AcquireSession(const AcquireSessionInput &in, AcquireSessionResult *out) {
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

    SessionRecord old;
    const bool had = LoadSession(in.player_id, &old);
    if (had)
        ExpireIfGraceElapsed(in.player_id, &old);

    const bool kick = in.kick_other_device;
    if (had && old.state == SessionState::Online && !kick && old.device_id != in.device_id) {
        out->message = "already logged in on another device";
        out->error_code = "ALREADY_ONLINE";
        return false;
    }

    SessionRecord rec;
    rec.token = GenHex(32);
    rec.session_id = GenHex(16);
    rec.server_id = in.server_id;
    rec.login_time_sec = NowUnixSec();
    rec.device_id = in.device_id;
    rec.state = SessionState::Online;
    rec.generation = had ? old.generation + 1 : 1;
    rec.gateway_id = in.gateway_instance_id;
    rec.connection_id = 0;
    rec.disconnect_deadline_sec = 0;
    if (!in.preferred_gamelogic_instance_id.empty()) {
        rec.gamelogic_instance_id = in.preferred_gamelogic_instance_id;
    } else if (!logic_instance_ids_.empty()) {
        rec.gamelogic_instance_id =
            logic_instance_ids_[static_cast<size_t>(in.player_id % logic_instance_ids_.size())];
    } else {
        rec.gamelogic_instance_id = "gl-0";
    }
    // 登录阶段尚未进图
    rec.map_instance_id = 0;
    rec.map_owner_epoch = 0;
    rec.route_version = 1;

    int ttl = static_cast<int>(in.ttl_sec);
    if (ttl <= 0)
        ttl = default_ttl_sec_;
    if (ttl > long_ttl_sec_)
        ttl = long_ttl_sec_;
    if (!SaveSession(in.player_id, rec, ttl)) {
        out->message = "redis save session failed";
        out->error_code = "SAVE_FAILED";
        return false;
    }

    out->ok = true;
    out->message = "login ok";
    out->session_id = rec.session_id;
    out->fence_token = rec.token;
    out->generation = rec.generation;
    out->gamelogic_instance_id = rec.gamelogic_instance_id;
    out->map_instance_id = rec.map_instance_id;
    out->map_owner_epoch = rec.map_owner_epoch;
    out->route_version = rec.route_version;
    out->kicked_previous = had;
    out->login_time_sec = rec.login_time_sec;
    out->server_id = rec.server_id;
    LOG_INFO << "SessionStore: AcquireSession player_id=" << in.player_id
             << " generation=" << rec.generation << " logic=" << rec.gamelogic_instance_id
             << " kicked=" << (had ? 1 : 0);
    return true;
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
    if (!rsp)
        return false;
    rsp->Clear();
    if (!available_) {
        rsp->set_ok(false);
        rsp->set_message("redis unavailable");
        return false;
    }
    if (req.player_id() == 0 || req.session_id().empty() || req.reconnect_ticket().empty()) {
        rsp->set_ok(false);
        rsp->set_message("player_id/session_id/reconnect_ticket required");
        return false;
    }
    SessionRecord rec;
    if (!LoadSession(req.player_id(), &rec)) {
        rsp->set_ok(false);
        rsp->set_message("session not found");
        return false;
    }
    if (ExpireIfGraceElapsed(req.player_id(), &rec)) {
        rsp->set_ok(false);
        rsp->set_message("reconnect grace expired");
        return false;
    }
    if (rec.state != SessionState::Disconnected && rec.state != SessionState::Online) {
        rsp->set_ok(false);
        rsp->set_message("session not reconnectable");
        return false;
    }
    if (rec.session_id != req.session_id()) {
        rsp->set_ok(false);
        rsp->set_message("session_id mismatch");
        return false;
    }
    if (rec.token != req.reconnect_ticket()) {
        rsp->set_ok(false);
        rsp->set_message("reconnect_ticket mismatch");
        return false;
    }

    // 轮换 fence，使旧 Gateway 迟到包失效
    rec.token = GenHex(32);
    rec.generation += 1;
    rec.state = SessionState::Online;
    rec.disconnect_deadline_sec = 0;
    rec.connection_id = 0;
    rec.gateway_id.clear();

    if (!SaveSession(req.player_id(), rec, default_ttl_sec_)) {
        rsp->set_ok(false);
        rsp->set_message("redis save failed");
        return false;
    }
    rsp->set_ok(true);
    rsp->set_message("reconnect ok");
    rsp->set_token(rec.token);
    rsp->set_session_id(rec.session_id);
    rsp->set_generation(rec.generation);
    LOG_INFO << "SessionStore: Reconnect player_id=" << req.player_id()
             << " generation=" << rec.generation;
    return true;
}

bool SessionStore::BindConnection(uint64_t player_id, const std::string &token,
                                  const std::string &gateway_id, int connection_id) {
    if (!available_ || player_id == 0 || token.empty())
        return false;
    SessionRecord rec;
    if (!LoadSession(player_id, &rec))
        return false;
    if (rec.token != token)
        return false;
    rec.gateway_id = gateway_id;
    rec.connection_id = connection_id;
    rec.state = SessionState::Online;
    rec.disconnect_deadline_sec = 0;
    return SaveSession(player_id, rec, default_ttl_sec_);
}

bool SessionStore::MarkDisconnected(uint64_t player_id, const std::string &token,
                                    uint64_t generation) {
    if (!available_ || player_id == 0)
        return false;
    SessionRecord rec;
    if (!LoadSession(player_id, &rec))
        return false;
    // 旧连接/旧世代：忽略，避免误杀新会话
    if (rec.token != token || rec.generation != generation) {
        LOG_INFO << "SessionStore: MarkDisconnected ignored player_id=" << player_id
                 << " (stale token/generation)";
        return false;
    }
    if (rec.state != SessionState::Online) {
        return true;
    }
    rec.state = SessionState::Disconnected;
    rec.disconnect_deadline_sec = NowUnixSec() + grace_sec_;
    rec.connection_id = 0;
    const bool ok = SaveSession(player_id, rec, default_ttl_sec_);
    LOG_INFO << "SessionStore: DISCONNECTED player_id=" << player_id
             << " grace_sec=" << grace_sec_ << " deadline=" << rec.disconnect_deadline_sec;
    return ok;
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
    SessionRecord rec;
    if (!LoadSession(req.player_id(), &rec)) {
        rsp->set_ok(true);
        rsp->set_message("already offline");
        return true;
    }
    if (!req.token().empty() && req.token() != rec.token) {
        rsp->set_ok(false);
        rsp->set_message("token mismatch");
        return false;
    }
    Client().Del(SessionKey(req.player_id()));
    rsp->set_ok(true);
    rsp->set_message("logout ok");
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
