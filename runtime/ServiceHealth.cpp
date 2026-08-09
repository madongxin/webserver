#include "ServiceHealth.h"

#include <chrono>
#include <sstream>

#ifndef GAMEMESH_GIT_SHA
#define GAMEMESH_GIT_SHA "unknown"
#endif
#ifndef GAMEMESH_BUILD_TIME
#define GAMEMESH_BUILD_TIME __DATE__ " " __TIME__
#endif

namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ServiceHealth &ServiceHealth::Instance() {
    static ServiceHealth g;
    return g;
}

void ServiceHealth::Configure(const std::string &service, const std::string &server_id) {
    service_ = service.empty() ? "gamemesh" : service;
    server_id_ = server_id.empty() ? "local" : server_id;
}

void ServiceHealth::SetReady(bool v) { ready_.store(v); }

void ServiceHealth::SetDraining(bool v) {
    draining_.store(v);
    if (v)
        ready_.store(false);
}

bool ServiceHealth::ready() const { return ready_.load() && !draining_.load(); }

void ServiceHealth::MarkAlive() {
    alive_.store(true);
    last_alive_unix_.store(NowUnix());
}

bool ServiceHealth::IsLive(int max_stale_sec) const {
    if (!alive_.load())
        return false;
    if (max_stale_sec <= 0)
        return true;
    const int64_t last = last_alive_unix_.load();
    if (last == 0)
        return true;  // 尚未心跳前视为活（启动窗口）
    return (NowUnix() - last) <= max_stale_sec;
}

std::string ServiceHealth::VersionJson() const {
    std::ostringstream os;
    os << "{\"service\":\"" << service_ << "\",\"server_id\":\"" << server_id_
       << "\",\"git_sha\":\"" << GAMEMESH_GIT_SHA << "\",\"build_time\":\"" << GAMEMESH_BUILD_TIME
       << "\",\"protocol\":\"game.proto+brpc\"}";
    return os.str();
}

std::string ServiceHealth::LivenessJson() const {
    const bool live = IsLive(30);
    std::ostringstream os;
    os << "{\"alive\":" << (live ? "true" : "false") << ",\"service\":\"" << service_
       << "\",\"last_alive_unix\":" << last_alive_unix_.load() << "}";
    return os.str();
}

std::string ServiceHealth::ReadinessJson(bool deps_ok, const std::string &deps_detail) const {
    const bool ok = ready() && deps_ok;
    std::ostringstream os;
    os << "{\"ready\":" << (ok ? "true" : "false") << ",\"draining\":"
       << (draining_.load() ? "true" : "false") << ",\"deps_ok\":" << (deps_ok ? "true" : "false")
       << ",\"detail\":\"" << deps_detail << "\"}";
    return os.str();
}
