#include "LoginAdmission.h"

#include <cstdlib>
#include <mutex>

namespace {

int EnvInt(const char *key, int fallback) {
    const char *v = std::getenv(key);
    if (!v || !*v)
        return fallback;
    return std::atoi(v);
}

std::mutex &AdmissionMu() {
    static std::mutex mu;
    return mu;
}

}  // namespace

LoginAdmission &LoginAdmission::Instance() {
    static LoginAdmission inst;
    return inst;
}

LoginAdmission::LoginAdmission() {
    max_inflight_ = EnvInt("GAMEMESH_LOGIN_INFLIGHT_MAX", 256);
    reconnect_reserve_ = EnvInt("GAMEMESH_LOGIN_RECONNECT_RESERVE", 32);
    if (reconnect_reserve_ < 0)
        reconnect_reserve_ = 0;
    if (max_inflight_ < 0)
        max_inflight_ = 0;
    if (reconnect_reserve_ > max_inflight_)
        reconnect_reserve_ = max_inflight_;
}

bool LoginAdmission::TryAcquire(bool reconnect) {
    std::lock_guard<std::mutex> lk(AdmissionMu());
    const int limit = reconnect ? max_inflight_ : (max_inflight_ - reconnect_reserve_);
    if (inflight_ >= limit)
        return false;
    ++inflight_;
    return true;
}

void LoginAdmission::Release() {
    std::lock_guard<std::mutex> lk(AdmissionMu());
    if (inflight_ > 0)
        --inflight_;
}
