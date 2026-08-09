#include "AuthLoginRateLimit.h"

#include "Logging.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_local_mu;
std::unordered_map<uint64_t, std::pair<int, int64_t>> g_local_fails;  // count, window_sec

int64_t NowSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool EnsureRedis() {
#ifdef WEBSERVER_ENABLE_REDIS
    if (RedisPool::Instance().ready())
        return true;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::ifstream in(RedisConfigPath::RedisCnf());
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                while (!s.empty() && s.front() == ' ')
                    s.erase(s.begin());
                while (!s.empty() && s.back() == ' ')
                    s.pop_back();
                return s;
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            if (key == "ip")
                host = val;
            else if (key == "port")
                port = std::atoi(val.c_str());
            else if (key == "password")
                password = val;
        }
    }
    return RedisPool::Instance().Init(host, port, password, 4);
#else
    return false;
#endif
}

std::string FailKey(const std::string &prefix, uint64_t player_id) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%sauth:login_fail:%llu", prefix.c_str(),
                  static_cast<unsigned long long>(player_id));
    return buf;
}

bool LocalLimited(uint64_t player_id, int window_sec, int max_fails) {
    const int64_t now = NowSec();
    std::lock_guard<std::mutex> lk(g_local_mu);
    auto &e = g_local_fails[player_id];
    if (now - e.second > window_sec)
        e = {0, now};
    return e.first >= max_fails;
}

void LocalRecord(uint64_t player_id, int window_sec) {
    const int64_t now = NowSec();
    std::lock_guard<std::mutex> lk(g_local_mu);
    auto &e = g_local_fails[player_id];
    if (now - e.second > window_sec)
        e = {0, now};
    e.first += 1;
    e.second = now;
}

void LocalClear(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(g_local_mu);
    g_local_fails.erase(player_id);
}

}  // namespace

AuthLoginRateLimit &AuthLoginRateLimit::Instance() {
    static AuthLoginRateLimit g;
    return g;
}

void AuthLoginRateLimit::Configure(const std::string &key_prefix, int window_sec, int max_fails) {
    if (!key_prefix.empty())
        key_prefix_ = key_prefix;
    if (window_sec > 0)
        window_sec_ = window_sec;
    if (max_fails > 0)
        max_fails_ = max_fails;
}

bool AuthLoginRateLimit::IsLimited(uint64_t player_id) {
    if (player_id == 0)
        return false;
#ifdef WEBSERVER_ENABLE_REDIS
    if (EnsureRedis()) {
        auto lease = RedisPool::Instance().Acquire();
        if (lease) {
            const std::string key = FailKey(key_prefix_, player_id);
            std::vector<std::string> out;
            // GET count；无 key 视为 0
            static const char *kGet = "return redis.call('GET', KEYS[1]) or '0'";
            if (lease->Eval(kGet, {key}, {}, &out) && !out.empty()) {
                const int n = std::atoi(out[0].c_str());
                return n >= max_fails_;
            }
        }
    }
#endif
    return LocalLimited(player_id, window_sec_, max_fails_);
}

void AuthLoginRateLimit::RecordFail(uint64_t player_id) {
    if (player_id == 0)
        return;
#ifdef WEBSERVER_ENABLE_REDIS
    if (EnsureRedis()) {
        auto lease = RedisPool::Instance().Acquire();
        if (lease) {
            const std::string key = FailKey(key_prefix_, player_id);
            std::vector<std::string> out;
            static const char *kIncr =
                "local c=redis.call('INCR', KEYS[1]); if c==1 then redis.call('EXPIRE', KEYS[1], "
                "tonumber(ARGV[1])) end; return c";
            if (lease->Eval(kIncr, {key}, {std::to_string(window_sec_)}, &out))
                return;
            LOG_WARN << "AuthLoginRateLimit Redis INCR failed; local fallback";
        }
    }
#endif
    LocalRecord(player_id, window_sec_);
}

void AuthLoginRateLimit::Clear(uint64_t player_id) {
    if (player_id == 0)
        return;
#ifdef WEBSERVER_ENABLE_REDIS
    if (EnsureRedis()) {
        auto lease = RedisPool::Instance().Acquire();
        if (lease) {
            lease->Del(FailKey(key_prefix_, player_id));
        }
    }
#endif
    LocalClear(player_id);
}
