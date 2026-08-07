#include "IServiceRegistry.h"

#include "EtcdDiscovery.h"
#include "Logging.h"

#include <cstdlib>

StaticServiceRegistry &StaticServiceRegistry::Get() {
    static StaticServiceRegistry g;
    return g;
}

void StaticServiceRegistry::SetStatic(const std::string &service,
                                      std::vector<ServiceInstance> instances) {
    std::lock_guard<std::mutex> lk(mu_);
    by_service_[service] = std::move(instances);
}

void StaticServiceRegistry::SetStaticAddrs(const std::string &service,
                                           const std::vector<std::string> &addrs,
                                           const std::vector<std::string> &instance_ids) {
    std::vector<ServiceInstance> insts;
    for (size_t i = 0; i < addrs.size(); ++i) {
        ServiceInstance in;
        in.service = service;
        in.instance_id =
            i < instance_ids.size() ? instance_ids[i] : (service + "-" + std::to_string(i));
        in.address = addrs[i];
        const auto colon = addrs[i].rfind(':');
        if (colon != std::string::npos)
            in.port = std::atoi(addrs[i].c_str() + colon + 1);
        in.status = "UP";
        insts.push_back(std::move(in));
    }
    SetStatic(service, std::move(insts));
}

bool StaticServiceRegistry::RegisterInstance(const ServiceInstance &inst, int /*ttl_sec*/) {
    std::lock_guard<std::mutex> lk(mu_);
    auto &vec = by_service_[inst.service];
    for (auto &x : vec) {
        if (x.instance_id == inst.instance_id) {
            x = inst;
            return true;
        }
    }
    vec.push_back(inst);
    return true;
}

bool StaticServiceRegistry::Discover(const std::string &service,
                                     std::vector<ServiceInstance> *out) {
    if (!out)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_service_.find(service);
    if (it == by_service_.end() || it->second.empty())
        return false;
    *out = it->second;
    return true;
}

bool StaticServiceRegistry::DiscoverAddrs(const std::string &service,
                                          std::vector<std::string> *addrs) {
    std::vector<ServiceInstance> insts;
    if (!Discover(service, &insts) || !addrs)
        return false;
    addrs->clear();
    for (const auto &i : insts)
        addrs->push_back(i.address);
    return !addrs->empty();
}

ServiceRegistryFacade &ServiceRegistryFacade::Get() {
    static ServiceRegistryFacade g;
    return g;
}

IServiceRegistry &ServiceRegistryFacade::Active() {
    (void)static_only_;
    if (!static_only_ && EtcdDiscovery::Instance().enabled()) {
        // etcd 地址由 Bootstrap 同步进 Static；此处仍返回 Static 读写面
    }
    return StaticServiceRegistry::Get();
}
