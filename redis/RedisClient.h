#pragma once

/**
 * @file RedisClient.h
 * @brief Redis 同步客户端封装（基于 hiredis）
 *
 * =============================================================================
 * 模块定位
 * =============================================================================
 *
 *   RedisClient 是对 hiredis 的轻量 C++ 包装，提供连接管理与常用 KV/Hash 命令。
 *   当前主要用于 SessionStore：玩家登录态以 Hash 形式存于 Redis，key 形如
 *   `game:session:{player_id}`，字段包括 token / deviceId / serverId / loginTime。
 *
 *   SessionStore::InitFromConfig()
 *     -> 读取 config/redis.cnf
 *     -> RedisClient::Connect(host, port, password)
 *     -> HSet / HGetAll / Expire / Exists / Del 维护会话
 *
 * =============================================================================
 * 设计要点
 * =============================================================================
 *
 * - 同步阻塞：每条命令通过 redisCommand / redisCommandArgv 发送并等待回复；
 *   适合登录、鉴权等低频路径，不适合高 QPS 热路径。
 * - 单连接：内部仅持有一个 redisContext*（ctx_），非线程安全；
 *   SessionStore 通过进程内 static RedisClient 单例使用。
 * - 连接超时：Connect 使用 2 秒超时（见 RedisClient.cpp）。
 * - 返回值约定：bool 表示「命令是否执行成功」；Exists / Expire 额外区分
 *   key 是否存在 / TTL 是否设置成功（integer == 1）。
 * - ctx_ 以 void* 存储，避免在头文件暴露 hiredis 类型依赖。
 *
 * =============================================================================
 * 典型用法
 * =============================================================================
 *
 *   RedisClient redis;
 *   if (!redis.Connect("127.0.0.1", 6379, "")) { ... }
 *
 *   std::map<std::string, std::string> fields{{"token", "abc"}, {"deviceId", "d1"}};
 *   redis.HSet("game:session:1001", fields);
 *   redis.Expire("game:session:1001", 7200);
 *
 *   std::map<std::string, std::string> out;
 *   redis.HGetAll("game:session:1001", &out);
 */

#include <map>
#include <string>
#include <vector>

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();

    RedisClient(const RedisClient &) = delete;
    RedisClient &operator=(const RedisClient &) = delete;

    /**
     * 建立 TCP 连接并完成 AUTH（若 password 非空）与 PING 探活。
     * 若已有连接会先 Disconnect；失败时 ctx_ 保持为空。
     *
     * @return true  连接、认证（如需）、PING 均成功
     * @return false 连接超时、AUTH 失败或 PING 非 PONG
     */
    bool Connect(const std::string &host, int port, const std::string &password);

    /** 释放 redisContext，之后 IsConnected() 为 false */
    void Disconnect();

    /** 是否已持有有效连接（ctx_ != nullptr） */
    bool IsConnected() const { return ctx_ != nullptr; }

    /** 发送 PING，期望回复 PONG；用于探活与 Connect 末尾校验 */
    bool Ping();

    /**
     * 判断 key 是否存在。
     * @return true  key 存在（EXISTS 返回 1）
     * @return false key 不存在、未连接或命令异常
     */
    bool Exists(const std::string &key);

    /** 删除 key；失败或未连接返回 false */
    bool Del(const std::string &key);

    /**
     * 为 key 设置过期时间（秒）。
     * @param ttl_sec 必须 > 0
     * @return true  key 存在且 EXPIRE 成功（返回 1）
     * @return false key 不存在、参数非法或未连接
     */
    bool Expire(const std::string &key, int ttl_sec);

    /**
     * 批量写入 Hash 字段（HSET key field value ...）。
     * 使用 redisCommandArgv 避免字段值含空格时的拼接问题。
     *
     * @param fields 非空；同一 field 多次出现的行为由 Redis 服务端决定
     * @return false fields 为空、未连接或收到 ERROR 回复
     */
    bool HSet(const std::string &key, const std::map<std::string, std::string> &fields);

    /**
     * 读取 Hash 全部字段（HGETALL）。
     *
     * @param fields 输出参数，调用前会被 clear；key 不存在时返回 true 且 *fields 为空
     * @return false 未连接、fields 为 nullptr 或回复格式异常
     */
    bool HGetAll(const std::string &key, std::map<std::string, std::string> *fields);

    /**
     * EVAL script numkeys key [key ...] arg [arg ...]
     * 期望回复为字符串数组（或单字符串 / 整数，统一转为 out 元素）。
     */
    bool Eval(const std::string &script, const std::vector<std::string> &keys,
              const std::vector<std::string> &args, std::vector<std::string> *out);

private:
    /**
     * 校验 hiredis 回复：非 REDIS_REPLY_ERROR 视为成功，并 freeReplyObject。
     * @param reply 可为 nullptr（视为失败）
     */
    bool CheckReply(void *reply, const char *op);

    /** hiredis redisContext*，以 void* 隐藏头文件对 hiredis 的依赖 */
    void *ctx_ = nullptr;
};
