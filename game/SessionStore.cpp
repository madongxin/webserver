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

std::string GenToken() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char hex[] = "0123456789abcdef";
    std::string s(32, '0');
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
                 int *ttl, int *long_ttl) {
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

bool SessionStore::InitFromConfig() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int ttl = 7200;
    int long_ttl = 86400;
    if (!ParseConfig(path, &host, &port, &password, &ttl, &long_ttl)) {
        LOG_ERROR << "SessionStore: cannot read " << path;
        available_ = false;
        return false;
    }
    if (ttl > 0)
        default_ttl_sec_ = ttl;
    if (long_ttl > 0)
        long_ttl_sec_ = long_ttl;
    if (!Client().Connect(host, port, password)) {
        LOG_ERROR << "SessionStore: Redis connect failed " << host << ":" << port;
        available_ = false;
        return false;
    }
    available_ = true;
    LOG_INFO << "SessionStore: Redis ok " << host << ":" << port
             << " default_ttl_sec=" << default_ttl_sec_;
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
    out->device_id = fields["deviceId"];
    out->server_id = static_cast<uint32_t>(std::strtoul(fields["serverId"].c_str(), nullptr, 10));
    out->login_time_sec =
        static_cast<int64_t>(std::strtoll(fields["loginTime"].c_str(), nullptr, 10));
    return !out->token.empty();
}

bool SessionStore::SaveSession(uint64_t player_id, const SessionRecord &rec, int ttl_sec) {
    if (!available_ || ttl_sec <= 0)
        return false;
    const std::string key = SessionKey(player_id);
    std::map<std::string, std::string> fields;
    fields["token"] = rec.token;
    fields["serverId"] = std::to_string(rec.server_id);
    fields["loginTime"] = std::to_string(rec.login_time_sec);
    fields["deviceId"] = rec.device_id;
    if (!Client().HSet(key, fields))
        return false;
    return Client().Expire(key, ttl_sec);
}

bool SessionStore::Login(const game::LoginReq &req, game::LoginRsp *rsp) {
    if (!rsp)
        return false;
    rsp->Clear();
    if (!available_) {
        rsp->set_ok(false);
        rsp->set_message("redis session store unavailable");
        return false;
    }
    if (req.player_id() == 0 || req.device_id().empty()) {
        rsp->set_ok(false);
        rsp->set_message("player_id and device_id required");
        return false;
    }
    SessionRecord old;
    const bool had = LoadSession(req.player_id(), &old);
    const bool kick = req.kick_other_device();
    if (had && !kick && old.device_id != req.device_id()) {
        rsp->set_ok(false);
        rsp->set_message("already logged in on another device");
        return false;
    }
    SessionRecord rec;
    rec.token = GenToken();
    rec.server_id = req.server_id();
    rec.login_time_sec = NowUnixSec();
    rec.device_id = req.device_id();
    int ttl = static_cast<int>(req.ttl_sec());
    if (ttl <= 0)
        ttl = default_ttl_sec_;
    if (ttl > long_ttl_sec_)
        ttl = long_ttl_sec_;
    if (!SaveSession(req.player_id(), rec, ttl)) {
        rsp->set_ok(false);
        rsp->set_message("redis save session failed");
        return false;
    }
    rsp->set_ok(true);
    rsp->set_message("login ok");
    rsp->set_token(rec.token);
    rsp->set_server_id(rec.server_id);
    rsp->set_login_time_sec(rec.login_time_sec);
    rsp->set_kicked_previous(had && kick);
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
    const bool online = LoadSession(req.player_id(), &rec);
    rsp->set_online(online);
    if (!online) {
        rsp->set_ok(true);
        rsp->set_valid(false);
        rsp->set_message("not online");
        return true;
    }
    const bool valid = !req.token().empty() && req.token() == rec.token;
    rsp->set_ok(true);
    rsp->set_valid(valid);
    rsp->set_server_id(rec.server_id);
    rsp->set_device_id(rec.device_id);
    rsp->set_login_time_sec(rec.login_time_sec);
    rsp->set_message(valid ? "token ok" : "token mismatch");
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

bool SessionStore::IsPlayerOnline(uint64_t player_id) const {
    if (!available_ || player_id == 0)
        return false;
    return Client().Exists(SessionKey(player_id));
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
    if (rec.token != token) {
        if (err)
            *err = "invalid session token";
        return false;
    }
    return true;
}
