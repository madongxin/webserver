#include "HealthDeps.h"

#include "FormalMode.h"
#include "HealthProbe.h"
#include "ServiceHealth.h"

#include <cstdlib>
#include <cstring>

namespace {

bool EnvForceNotReady() {
    const char *v = std::getenv("GAMEMESH_FORCE_NOT_READY");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

}  // namespace

HealthDepsResult EvaluateHealthDeps(const std::string &role) {
    HealthDepsResult r;
    if (EnvForceNotReady()) {
        r.ok = false;
        r.detail = "GAMEMESH_FORCE_NOT_READY";
        return r;
    }
    if (ServiceHealth::Instance().draining()) {
        r.ok = false;
        r.detail = "draining";
        return r;
    }

    auto snap = HealthProbeStore::Instance().Current();
    if (!snap || snap->updated_unix_ms == 0) {
        HealthProbeStore::Instance().Refresh(role);
        snap = HealthProbeStore::Instance().Current();
    }
    if (!HealthProbeStore::Instance().Fresh(15000)) {
        r.ok = false;
        r.detail = "health snapshot stale";
        return r;
    }
    r.ok = snap && snap->ok;
    r.detail = snap ? snap->detail : "no snapshot";
    (void)FormalModeEnabled();
    return r;
}
