#pragma once

#include <atomic>
#include <cstdint>
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

    /** EventLoop 心跳：刷新 alive 与 last_alive_unix */
    void MarkAlive();
    /** 超过 max_stale_sec 未 MarkAlive 则视为不存活（默认 30s） */
    bool IsLive(int max_stale_sec = 30) const;

    /** 是否接受新登录/进图（非 draining） */
    bool AcceptsNewWork() const { return !draining_.load() && ready_.load(); }

    int64_t last_alive_unix() const { return last_alive_unix_.load(); }

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
    std::atomic<int64_t> last_alive_unix_{0};
};
