#pragma once

#include <cstdint>
#include <string>

/** Auth 侧 access_token 存 Redis；与 Session fence_token 分离，不创建在线 Session。 */
class AuthTokenStore {
public:
    static AuthTokenStore &Instance();

    /** 复用 SessionStore 已连接的 Redis（同进程）；或自行 InitFromConfig。 */
    bool InitFromConfig();
    bool Available() const { return available_; }

    bool IssueAccessToken(uint64_t player_id, uint64_t account_id, int ttl_sec,
                          std::string *access_token_out);
    bool VerifyAccessToken(uint64_t player_id, const std::string &access_token, std::string *err);
    bool RefreshAccessToken(uint64_t player_id, const std::string &old_token, int ttl_sec,
                            std::string *new_token_out, std::string *err);

private:
    AuthTokenStore() = default;
    std::string TokenKey(const std::string &token) const;
    std::string PlayerKey(uint64_t player_id) const;
    bool available_ = false;
    int default_ttl_sec_ = 3600;
};
