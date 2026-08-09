#include "IServiceRegistry.h"

#include "AdvertiseAddr.h"
#include "Logging.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

StaticServiceRegistry &StaticServiceRegistry::Get() {
    static StaticServiceRegistry g;
    return g;
}

void StaticServiceRegistry::SetStatic(const std::string &service,
                                      std::vector<ServiceInstance> instances) {
    std::lock_guard<std::mutex> lk(mu_);
    // 空列表不得清空已有健康节点（降级保护）
    if (instances.empty()) {
        LOG_WARN << "StaticServiceRegistry: ignore empty SetStatic for " << service;
        return;
    }
    by_service_[service] = std::move(instances);
}

void StaticServiceRegistry::SetStaticAddrs(const std::string &service,
                                           const std::vector<std::string> &addrs,
                                           const std::vector<std::string> &instance_ids) {
    std::vector<ServiceInstance> insts;
    for (size_t i = 0; i < addrs.size(); ++i) {
        if (addrs[i].empty())
            continue;
        std::string err;
        if (!ValidateAdvertiseAddr(addrs[i], &err)) {
            LOG_WARN << "StaticServiceRegistry: skip bad addr " << addrs[i] << " (" << err << ")";
            continue;
        }
        ServiceInstance in;
        in.service = service;
        in.instance_id =
            i < instance_ids.size() && !instance_ids[i].empty()
                ? instance_ids[i]
                : (service + "-" + std::to_string(i));
        in.address = addrs[i];
        const auto colon = addrs[i].rfind(':');
        if (colon != std::string::npos)
            in.port = std::atoi(addrs[i].c_str() + colon + 1);
        in.status = "UP";
        in.protocol = "baidu_std";
        insts.push_back(std::move(in));
    }
    SetStatic(service, std::move(insts));
}

bool StaticServiceRegistry::RegisterInstance(const ServiceInstance &inst, int ttl_sec) {
    std::string err;
    if (!ValidateAdvertiseAddr(inst.address, &err)) {
        LOG_ERROR << "RegisterInstance reject: " << err;
        return false;
    }
    if (inst.instance_id.empty() || inst.service.empty()) {
        LOG_ERROR << "RegisterInstance reject: empty service/instance_id";
        return false;
    }
    ServiceInstance copy = inst;
    if (copy.status.empty())
        copy.status = "UP";
    if (ttl_sec > 0)
        copy.expire_at_ms = NowMs() + static_cast<int64_t>(ttl_sec) * 1000;
    std::lock_guard<std::mutex> lk(mu_);
    auto &vec = by_service_[copy.service];
    for (auto &x : vec) {
        if (x.instance_id == copy.instance_id) {
            x = copy;
            return true;
        }
    }
    vec.push_back(copy);
    return true;
}

bool StaticServiceRegistry::UnregisterInstance(const std::string &service,
                                               const std::string &instance_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_service_.find(service);
    if (it == by_service_.end())
        return false;
    auto &vec = it->second;
    for (auto i = vec.begin(); i != vec.end(); ++i) {
        if (i->instance_id == instance_id) {
            vec.erase(i);
            return true;
        }
    }
    return false;
}

bool StaticServiceRegistry::SetInstanceStatus(const std::string &service,
                                              const std::string &instance_id,
                                              const std::string &status) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_service_.find(service);
    if (it == by_service_.end())
        return false;
    for (auto &x : it->second) {
        if (x.instance_id == instance_id) {
            x.status = status;
            return true;
        }
    }
    return false;
}

bool StaticServiceRegistry::RenewInstance(const std::string &service,
                                          const std::string &instance_id, int ttl_sec) {
    if (ttl_sec <= 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_service_.find(service);
    if (it == by_service_.end())
        return false;
    for (auto &x : it->second) {
        if (x.instance_id == instance_id) {
            x.expire_at_ms = NowMs() + static_cast<int64_t>(ttl_sec) * 1000;
            return true;
        }
    }
    return false;
}

void StaticServiceRegistry::PurgeExpiredUnlocked() {
    const int64_t now = NowMs();
    for (auto &kv : by_service_) {
        auto &vec = kv.second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [now](const ServiceInstance &i) {
                                     return i.expire_at_ms > 0 && i.expire_at_ms <= now;
                                 }),
                  vec.end());
    }
}

bool StaticServiceRegistry::DiscoverAll(const std::string &service,
                                        std::vector<ServiceInstance> *out) {
    if (!out)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    PurgeExpiredUnlocked();
    auto it = by_service_.find(service);
    if (it == by_service_.end() || it->second.empty())
        return false;
    *out = it->second;
    return true;
}

bool StaticServiceRegistry::Discover(const std::string &service,
                                     std::vector<ServiceInstance> *out) {
    if (!out)
        return false;
    std::vector<ServiceInstance> all;
    if (!DiscoverAll(service, &all))
        return false;
    out->clear();
    for (const auto &i : all) {
        if (i.status == "UP" || i.status.empty())
            out->push_back(i);
    }
    return !out->empty();
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
    // 生产路径：Static（含静态 *_addrs 与进程内注册）。etcd v2 不得作为 Active。
    return StaticServiceRegistry::Get();
}
