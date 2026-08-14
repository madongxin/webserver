#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct HealthProbeSnapshot {
    bool ok = false;
    std::string detail = "uninitialized";
    int64_t updated_unix_ms = 0;
    int consecutive_failures = 0;
    int consecutive_successes = 0;
};

class HealthProbeStore {
public:
    static HealthProbeStore &Instance();

    void Refresh(const std::string &role);
    std::shared_ptr<const HealthProbeSnapshot> Current() const;
    bool Fresh(int max_age_ms = 15000) const;

    static void SetGatewayTcpListening(bool v);
    static bool GatewayTcpListening();

private:
    HealthProbeStore() = default;
    std::shared_ptr<const HealthProbeSnapshot> snap_{std::make_shared<HealthProbeSnapshot>()};
};
