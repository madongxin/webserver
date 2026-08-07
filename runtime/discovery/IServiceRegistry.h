#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** 服务发现抽象：业务禁止直接依赖硬编码地址列表。 */
class IServiceRegistry {
public:
    virtual ~IServiceRegistry() = default;

    struct ServiceInstance {
        std::string service;
        std::string instance_id;
        std::string address;  // host:port
        int port = 0;
        std::string version;
        int capacity = 0;
        std::string status;  // UP/DOWN
    };

    virtual bool RegisterInstance(const ServiceInstance &inst, int ttl_sec = 30) = 0;
    virtual bool Discover(const std::string &service, std::vector<ServiceInstance> *out) = 0;
    virtual bool DiscoverAddrs(const std::string &service, std::vector<std::string> *addrs) = 0;
};

/** 静态配置 fallback（etcd 未启用时）。 */
class StaticServiceRegistry : public IServiceRegistry {
public:
    static StaticServiceRegistry &Get();

    void SetStatic(const std::string &service, std::vector<ServiceInstance> instances);
    void SetStaticAddrs(const std::string &service, const std::vector<std::string> &addrs,
                        const std::vector<std::string> &instance_ids = {});

    bool RegisterInstance(const ServiceInstance &inst, int ttl_sec = 30) override;
    bool Discover(const std::string &service, std::vector<ServiceInstance> *out) override;
    bool DiscoverAddrs(const std::string &service, std::vector<std::string> *addrs) override;

private:
    StaticServiceRegistry() = default;
    std::mutex mu_;
    std::unordered_map<std::string, std::vector<ServiceInstance>> by_service_;
};

/** 全局入口：优先 etcd Discover，失败回退 Static。 */
class ServiceRegistryFacade {
public:
    static ServiceRegistryFacade &Get();
    IServiceRegistry &Active();
    void UseStaticOnly(bool v) { static_only_ = v; }

private:
    bool static_only_ = true;
};
