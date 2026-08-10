#include "RedisServiceRegistry.h"

#include "AdvertiseAddr.h"
#include "Logging.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <cstdlib>
#include <fstream>
#include <map>

namespace {

bool EnsurePool() {
#ifdef WEBSERVER_ENABLE_REDIS
    if (RedisPool::Instance().ready())
        return true;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::ifstream in(RedisConfigPath::RedisCnf());
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                while (!s.empty() && s.front() == ' ')
                    s.erase(s.begin());
                while (!s.empty() && s.back() == ' ')
                    s.pop_back();
                return s;
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            if (key == "ip")
                host = val;
            else if (key == "port")
                port = std::atoi(val.c_str());
            else if (key == "password")
                password = val;
        }
    }
    return RedisPool::Instance().Init(host, port, password, 4);
#else
    return false;
#endif
}

}  // namespace

RedisServiceRegistry &RedisServiceRegistry::Get() {
    static RedisServiceRegistry g;
    return g;
}

void RedisServiceRegistry::Configure(const std::string &key_prefix) {
    if (!key_prefix.empty())
        key_prefix_ = key_prefix;
}

bool RedisServiceRegistry::InitFromRedisConfig() {
#ifdef WEBSERVER_ENABLE_REDIS
    ready_ = EnsurePool();
    if (ready_)
        LOG_INFO << "RedisServiceRegistry ready prefix=" << key_prefix_;
    return ready_;
#else
    ready_ = false;
    return false;
#endif
}

std::string RedisServiceRegistry::InstanceKey(const std::string &service,
                                              const std::string &instance_id) const {
    return key_prefix_ + "svc:" + service + ":" + instance_id;
}

std::string IndexKey(const std::string &prefix, const std::string &service) {
    return prefix + "svcidx:" + service;
}

bool RedisServiceRegistry::RegisterInstance(const ServiceInstance &inst, int ttl_sec) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!ready_ && !InitFromRedisConfig())
        return false;
    std::string err;
    if (!ValidateAdvertiseAddr(inst.address, &err)) {
        LOG_ERROR << "RedisServiceRegistry reject: " << err;
        return false;
    }
    if (inst.service.empty() || inst.instance_id.empty())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> fields;
    fields["service"] = inst.service;
    fields["instance_id"] = inst.instance_id;
    fields["address"] = inst.address;
    fields["port"] = std::to_string(inst.port);
    fields["protocol"] = inst.protocol.empty() ? "baidu_std" : inst.protocol;
    fields["version"] = inst.version;
    fields["capacity"] = std::to_string(inst.capacity);
    fields["status"] = inst.status.empty() ? "UP" : inst.status;
    fields["metadata"] = inst.metadata;
    const std::string key = InstanceKey(inst.service, inst.instance_id);
    if (!lease->HSet(key, fields))
        return false;
    if (ttl_sec > 0 && !lease->Expire(key, ttl_sec))
        return false;
    // 索引集合：避免 Discover 无界全量 SCAN
    std::vector<std::string> out;
    static const char *kSadd = "return redis.call('SADD', KEYS[1], ARGV[1])";
    lease->Eval(kSadd, {IndexKey(key_prefix_, inst.service)}, {inst.instance_id}, &out);
    return true;
#else
    (void)inst;
    (void)ttl_sec;
    return false;
#endif
}

bool RedisServiceRegistry::UnregisterInstance(const std::string &service,
                                              const std::string &instance_id) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!ready_ && !InitFromRedisConfig())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> out;
    static const char *kSrem = "return redis.call('SREM', KEYS[1], ARGV[1])";
    lease->Eval(kSrem, {IndexKey(key_prefix_, service)}, {instance_id}, &out);
    return lease->Del(InstanceKey(service, instance_id));
#else
    (void)service;
    (void)instance_id;
    return false;
#endif
}

bool RedisServiceRegistry::SetInstanceStatus(const std::string &service,
                                             const std::string &instance_id,
                                             const std::string &status) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!ready_ && !InitFromRedisConfig())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::map<std::string, std::string> fields{{"status", status}};
    return lease->HSet(InstanceKey(service, instance_id), fields);
#else
    (void)service;
    (void)instance_id;
    (void)status;
    return false;
#endif
}

bool RedisServiceRegistry::Discover(const std::string &service, std::vector<ServiceInstance> *out) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!out)
        return false;
    out->clear();
    if (!ready_ && !InitFromRedisConfig())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    std::vector<std::string> ids;
    static const char *kMembers = "return redis.call('SMEMBERS', KEYS[1])";
    if (!lease->Eval(kMembers, {IndexKey(key_prefix_, service)}, {}, &ids))
        return false;
    for (const auto &id : ids) {
        const std::string key = InstanceKey(service, id);
        std::map<std::string, std::string> fields;
        if (!lease->HGetAll(key, &fields) || fields.empty()) {
            // 实例 TTL 过期：清索引残留
            std::vector<std::string> ignored;
            static const char *kSrem = "return redis.call('SREM', KEYS[1], ARGV[1])";
            lease->Eval(kSrem, {IndexKey(key_prefix_, service)}, {id}, &ignored);
            continue;
        }
        const std::string st = fields.count("status") ? fields["status"] : "UP";
        if (st != "UP")
            continue;
        ServiceInstance in;
        in.service = fields.count("service") ? fields["service"] : service;
        in.instance_id = fields.count("instance_id") ? fields["instance_id"] : "";
        in.address = fields.count("address") ? fields["address"] : "";
        in.port = fields.count("port") ? std::atoi(fields["port"].c_str()) : 0;
        in.protocol = fields.count("protocol") ? fields["protocol"] : "baidu_std";
        in.version = fields.count("version") ? fields["version"] : "";
        in.capacity = fields.count("capacity") ? std::atoi(fields["capacity"].c_str()) : 0;
        in.status = st;
        in.metadata = fields.count("metadata") ? fields["metadata"] : "";
        std::string err;
        if (in.instance_id.empty() || !ValidateAdvertiseAddr(in.address, &err))
            continue;
        out->push_back(std::move(in));
    }
    return true;
#else
    (void)service;
    (void)out;
    return false;
#endif
}

bool RedisServiceRegistry::DiscoverAddrs(const std::string &service,
                                         std::vector<std::string> *addrs) {
    if (!addrs)
        return false;
    std::vector<ServiceInstance> insts;
    if (!Discover(service, &insts))
        return false;
    addrs->clear();
    for (const auto &i : insts)
        addrs->push_back(i.address);
    return true;
}

bool RedisServiceRegistry::RenewInstance(const std::string &service,
                                         const std::string &instance_id, int ttl_sec) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (ttl_sec <= 0)
        return false;
    if (!ready_ && !InitFromRedisConfig())
        return false;
    auto lease = RedisPool::Instance().Acquire();
    if (!lease)
        return false;
    return lease->Expire(InstanceKey(service, instance_id), ttl_sec);
#else
    (void)service;
    (void)instance_id;
    (void)ttl_sec;
    return false;
#endif
}
