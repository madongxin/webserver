#pragma once

#include <string>

/**
 * 稳定 Gateway 实例身份：配置/环境注入，禁止从 0.0.0.0:port 拼接。
 * 解析优先级：显式参数 > GAMEMESH_INSTANCE_ID > gateway.cnf gateway_instance_id
 */
class GatewayIdentity {
public:
    static GatewayIdentity &Instance();

    /** 解析并缓存；empty 时返回 false（正式模式必须失败） */
    bool Resolve(std::string *err);
    /** 测试或显式注入 */
    void Set(const std::string &id);

    const std::string &id() const { return id_; }
    bool ready() const { return !id_.empty(); }

    /**
     * 可选：Redis SET NX 声明 instance_id，防止双进程撞 ID。
     * Redis 不可用时仅校验非空并返回 true。
     */
    bool ClaimOrFail(std::string *err);

private:
    GatewayIdentity() = default;
    std::string id_;
};

/** 供无类上下文快速读取（未 Resolve 时为空） */
inline const std::string &GatewayInstanceId() { return GatewayIdentity::Instance().id(); }
