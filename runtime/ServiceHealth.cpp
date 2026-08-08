#include "ServiceHealth.h"

#include <sstream>

#ifndef GAMEMESH_GIT_SHA
#define GAMEMESH_GIT_SHA "unknown"
#endif
#ifndef GAMEMESH_BUILD_TIME
#define GAMEMESH_BUILD_TIME __DATE__ " " __TIME__
#endif

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

void ServiceHealth::MarkAlive() { alive_.store(true); }

std::string ServiceHealth::VersionJson() const {
    std::ostringstream os;
    os << "{\"service\":\"" << service_ << "\",\"server_id\":\"" << server_id_
       << "\",\"git_sha\":\"" << GAMEMESH_GIT_SHA << "\",\"build_time\":\"" << GAMEMESH_BUILD_TIME
       << "\",\"protocol\":\"game.proto+brpc\"}";
    return os.str();
}

std::string ServiceHealth::LivenessJson() const {
    std::ostringstream os;
    os << "{\"alive\":" << (alive_.load() ? "true" : "false") << ",\"service\":\"" << service_
       << "\"}";
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
