#pragma once

#include <map>
#include <string>

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();

    RedisClient(const RedisClient &) = delete;
    RedisClient &operator=(const RedisClient &) = delete;

    bool Connect(const std::string &host, int port, const std::string &password);
    void Disconnect();
    bool IsConnected() const { return ctx_ != nullptr; }

    bool Ping();
    bool Exists(const std::string &key);
    bool Del(const std::string &key);
    bool Expire(const std::string &key, int ttl_sec);
    bool HSet(const std::string &key, const std::map<std::string, std::string> &fields);
    bool HGetAll(const std::string &key, std::map<std::string, std::string> *fields);

private:
    bool CheckReply(void *reply, const char *op);

    void *ctx_ = nullptr;
};
