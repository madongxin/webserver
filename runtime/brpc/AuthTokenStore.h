#pragma once

#include <cstdint>
#include <mutex>
#include <string>

/** Auth 侧 access/refresh 存 Redis（仅摘要）；与 Session fence_token 分离，不创建在线 Session。 */
class AuthTokenStore {
public:
    static AuthTokenStore &Instance();

    /** 复用 SessionStore 已连接的 Redis（同进程）；或自行 InitFromConfig。 */
    bool InitFromConfig();
    bool Available() const { return available_; }

    /** 签发分离的 access / refresh；Redis 只存 SHA-256 摘要。 */
    bool IssueTokenPair(uint64_t player_id, uint64_t account_id, int access_ttl_sec,
                        int refresh_ttl_sec, std::string *access_out, std::string *refresh_out);

    /** 兼容：只取 access（内部仍签发并丢弃 refresh 明文）。 */
    bool IssueAccessToken(uint64_t player_id, uint64_t account_id, int ttl_sec,
                          std::string *access_token_out);

    bool VerifyAccessToken(uint64_t player_id, const std::string &access_token, std::string *err);

    /**
     * 用 refresh_token 轮换：校验 refresh 摘要后签发新 pair，吊销旧 refresh。
     * new_refresh_out 可为 nullptr（仍会签发并吊销旧票）。
     */
    bool RefreshAccessToken(uint64_t player_id, const std::string &refresh_token, int access_ttl_sec,
                            std::string *new_access_out, std::string *err,
                            std::string *new_refresh_out = nullptr);

private:
    AuthTokenStore() = default;
    std::string DigestKey(const char *kind, const std::string &raw_token) const;
    std::string PlayerKey(uint64_t player_id) const;
    bool StoreTokenDigest(const char *kind, const std::string &raw_token, uint64_t player_id,
                          uint64_t account_id, int ttl_sec);
    bool VerifyTokenDigest(const char *kind, uint64_t player_id, const std::string &raw_token,
                           std::string *err, uint64_t *account_id_out);

    mutable std::mutex mu_;
    bool available_ = false;
    int default_ttl_sec_ = 3600;
    int default_refresh_ttl_sec_ = 86400 * 7;
};
