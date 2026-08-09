#pragma once

#include <cstdint>
#include <string>

/** 多 Auth 实例共享的登录失败限流（Redis；不可用时进程内降级） */
class AuthLoginRateLimit {
public:
    static AuthLoginRateLimit &Instance();

    void Configure(const std::string &key_prefix, int window_sec = 60, int max_fails = 10);
    bool IsLimited(uint64_t player_id);
    void RecordFail(uint64_t player_id);
    void Clear(uint64_t player_id);

private:
    AuthLoginRateLimit() = default;
    std::string key_prefix_ = "gamemesh:dev:";
    int window_sec_ = 60;
    int max_fails_ = 10;
};
