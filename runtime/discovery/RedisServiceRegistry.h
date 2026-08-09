#pragma once

#include "IServiceRegistry.h"

#include <atomic>
#include <string>

/**
 * Redis 动态发现（etcd v3 等价 MVP）：
 * key = {prefix}svc:{service}:{instance_id}，Hash + TTL；Discover 用 SCAN。
 * Watch 语义由调用方轮询 ApplySnapshot 实现。
 */
class RedisServiceRegistry : public IServiceRegistry {
public:
    static RedisServiceRegistry &Get();

    void Configure(const std::string &key_prefix);
    bool InitFromRedisConfig();
    bool ready() const { return ready_; }

    bool RegisterInstance(const ServiceInstance &inst, int ttl_sec = 30) override;
    bool UnregisterInstance(const std::string &service, const std::string &instance_id) override;
    bool SetInstanceStatus(const std::string &service, const std::string &instance_id,
                           const std::string &status) override;
    bool Discover(const std::string &service, std::vector<ServiceInstance> *out) override;
    bool DiscoverAddrs(const std::string &service, std::vector<std::string> *addrs) override;

    bool RenewInstance(const std::string &service, const std::string &instance_id, int ttl_sec);

private:
    RedisServiceRegistry() = default;
    std::string InstanceKey(const std::string &service, const std::string &instance_id) const;
    std::string key_prefix_ = "gamemesh:dev:";
    std::atomic<bool> ready_{false};
};
