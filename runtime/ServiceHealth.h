#pragma once

#include <atomic>
#include <mutex>
#include <string>

/** 进程级健康状态：liveness / readiness / version；SIGTERM 置 not-ready。 */
class ServiceHealth {
public:
    static ServiceHealth &Instance();

    void Configure(const std::string &service, const std::string &server_id);
    void SetReady(bool v);
    void SetDraining(bool v);
    bool ready() const;
    bool draining() const { return draining_.load(); }
    bool alive() const { return alive_.load(); }

    void MarkAlive();
    std::string VersionJson() const;
    std::string LivenessJson() const;
    std::string ReadinessJson(bool deps_ok, const std::string &deps_detail) const;

    const std::string &service() const { return service_; }
    const std::string &server_id() const { return server_id_; }

private:
    ServiceHealth() = default;
    std::string service_ = "gamemesh";
    std::string server_id_ = "local";
    std::atomic<bool> ready_{false};
    std::atomic<bool> draining_{false};
    std::atomic<bool> alive_{true};
};
