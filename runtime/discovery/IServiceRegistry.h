#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** 服务发现抽象：业务禁止直接依赖硬编码地址列表。 */
class IServiceRegistry {
public:
    virtual ~IServiceRegistry() = default;

    struct ServiceInstance {
        std::string service;       // service_name
        std::string instance_id;
        std::string address;       // advertise_addr host:port（必须可路由）
        int port = 0;
        std::string protocol = "baidu_std";
        std::string version;
        int capacity = 0;
        std::string status;  // UP / DRAINING / DOWN
        std::string lease_id;
        std::string metadata;
        /** 0 = 永不过期；>0 为绝对毫秒时间戳（RegisterInstance ttl） */
        int64_t expire_at_ms = 0;
    };

    virtual bool RegisterInstance(const ServiceInstance &inst, int ttl_sec = 30) = 0;
    virtual bool UnregisterInstance(const std::string &service, const std::string &instance_id) = 0;
    virtual bool SetInstanceStatus(const std::string &service, const std::string &instance_id,
                                   const std::string &status) = 0;
    /** 默认只返回 UP（不含 DRAINING/DOWN） */
    virtual bool Discover(const std::string &service, std::vector<ServiceInstance> *out) = 0;
    virtual bool DiscoverAddrs(const std::string &service, std::vector<std::string> *addrs) = 0;
};

/** 静态配置 + 进程内注册（etcd 可选；生产勿走 etcd v2）。 */
class StaticServiceRegistry : public IServiceRegistry {
public:
    static StaticServiceRegistry &Get();

    void SetStatic(const std::string &service, std::vector<ServiceInstance> instances);
    void SetStaticAddrs(const std::string &service, const std::vector<std::string> &addrs,
                        const std::vector<std::string> &instance_ids = {});

    bool RegisterInstance(const ServiceInstance &inst, int ttl_sec = 30) override;
    bool UnregisterInstance(const std::string &service, const std::string &instance_id) override;
    bool SetInstanceStatus(const std::string &service, const std::string &instance_id,
                           const std::string &status) override;
    bool Discover(const std::string &service, std::vector<ServiceInstance> *out) override;
    bool DiscoverAddrs(const std::string &service, std::vector<std::string> *addrs) override;

    /** 续租（进程内 lease）；ttl_sec>0 */
    bool RenewInstance(const std::string &service, const std::string &instance_id, int ttl_sec);

    /** 含 DRAINING，供运维/调试 */
    bool DiscoverAll(const std::string &service, std::vector<ServiceInstance> *out);

private:
    StaticServiceRegistry() = default;
    void PurgeExpiredUnlocked();
    std::mutex mu_;
    std::unordered_map<std::string, std::vector<ServiceInstance>> by_service_;
};

/** 全局入口：静态为主；etcd v2 不得成为生产 Active 实现。 */
class ServiceRegistryFacade {
public:
    static ServiceRegistryFacade &Get();
    IServiceRegistry &Active();
    void UseStaticOnly(bool v) { static_only_ = v; }

private:
    bool static_only_ = true;
};
