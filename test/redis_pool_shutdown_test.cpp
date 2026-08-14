/**
 * RedisPool：CLOSING 拒绝 Acquire；Shutdown 等待 lease；旧 lease 不得归还到新 generation。
 */
#include "Logging.h"
#include "RedisClient.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitFromCnf() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::ifstream in(path);
    if (!in)
        return false;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "ip")
            host = val;
        else if (key == "port")
            port = std::atoi(val.c_str());
        else if (key == "password")
            password = val;
    }
    return RedisPool::Instance().Init(host, port, password, 2);
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    RedisPool::Instance().Shutdown();
    if (!InitFromCnf()) {
        std::printf("FAIL redis init\n");
        return 1;
    }
    const uint64_t gen1 = RedisPool::Instance().generation();
    {
        auto lease = RedisPool::Instance().Acquire();
        if (!lease) {
            std::printf("FAIL acquire\n");
            return 1;
        }
        if (RedisPool::Instance().active_leases() < 1) {
            std::printf("FAIL active_leases\n");
            return 1;
        }
        std::atomic<bool> done{false};
        std::thread th([&]() {
            RedisPool::Instance().Shutdown(std::chrono::milliseconds(2000));
            done.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto denied = RedisPool::Instance().Acquire(50);
        if (denied) {
            std::printf("FAIL acquire during closing\n");
            lease = RedisPool::Lease();
            th.join();
            return 1;
        }
        lease = RedisPool::Lease();
        th.join();
        if (!done.load()) {
            std::printf("FAIL shutdown thread\n");
            return 1;
        }
    }
    if (!InitFromCnf()) {
        std::printf("FAIL reinit\n");
        return 1;
    }
    const uint64_t gen2 = RedisPool::Instance().generation();
    if (gen2 <= gen1) {
        std::printf("FAIL generation not bumped\n");
        return 1;
    }
    auto lease2 = RedisPool::Instance().Acquire();
    if (!lease2 || !lease2->Ping()) {
        std::printf("FAIL new generation acquire\n");
        return 1;
    }
    lease2 = RedisPool::Lease();
    RedisPool::Instance().Shutdown();
    std::printf("OK redis_pool_shutdown_test gen1=%llu gen2=%llu\n",
                static_cast<unsigned long long>(gen1), static_cast<unsigned long long>(gen2));
    return 0;
}
