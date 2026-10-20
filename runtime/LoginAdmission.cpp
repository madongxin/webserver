#include "LoginAdmission.h"

#include "ServerStats.h"

#include <cstdlib>
#include <mutex>
#include <utility>

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

int ClampCap(int n) {
    if (n < 1)
        return 1;
    if (n > 100000)
        return 100000;
    return n;
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
    const int base = max_inflight_ > 0 ? max_inflight_ : 256;
    stage_max_[0] = ClampCap(EnvInt("GAMEMESH_LOGIN_AUTH_INFLIGHT", base));
    stage_max_[1] = ClampCap(EnvInt("GAMEMESH_LOGIN_SESSION_INFLIGHT", stage_max_[0]));
    stage_max_[2] = ClampCap(EnvInt("GAMEMESH_LOGIN_BIND_INFLIGHT", stage_max_[0]));
    stage_max_[3] = ClampCap(EnvInt("GAMEMESH_LOGIN_ENTER_INFLIGHT", stage_max_[0]));
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

LoginAdmission::StagePermit::StagePermit(StagePermit &&other) noexcept
    : stage_(other.stage_), held_(other.held_) {
    other.held_ = false;
}

LoginAdmission::StagePermit LoginAdmission::StagePermit::Try(Stage stage, bool reconnect) {
    auto &adm = LoginAdmission::Instance();
    const int idx = static_cast<int>(stage);
    std::lock_guard<std::mutex> lk(AdmissionMu());
    const int limit = adm.stage_max_[idx];
    const int reserve = adm.reconnect_reserve_ < limit ? adm.reconnect_reserve_ : 0;
    const int usable = reconnect ? limit : (limit - reserve);
    if (adm.stage_inflight_[idx] >= usable) {
        if (stage == Stage::Auth)
            ServerStats::login_stage_reject_auth.fetch_add(1, std::memory_order_relaxed);
        else if (stage == Stage::Session)
            ServerStats::login_stage_reject_session.fetch_add(1, std::memory_order_relaxed);
        else if (stage == Stage::Bind)
            ServerStats::login_stage_reject_bind.fetch_add(1, std::memory_order_relaxed);
        else
            ServerStats::login_stage_reject_enter.fetch_add(1, std::memory_order_relaxed);
        return StagePermit();
    }
    ++adm.stage_inflight_[idx];
    return StagePermit(stage, true);
}

LoginAdmission::StagePermit::~StagePermit() {
    if (!held_)
        return;
    std::lock_guard<std::mutex> lk(AdmissionMu());
    auto &adm = LoginAdmission::Instance();
    const int idx = static_cast<int>(stage_);
    if (idx >= 0 && idx < 4 && adm.stage_inflight_[idx] > 0)
        --adm.stage_inflight_[idx];
    held_ = false;
}
