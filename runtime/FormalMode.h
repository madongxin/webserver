#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

/** 正式模式：GAMEMESH_FORMAL=1 时 fail-closed（禁止 Auth 本地降级、禁止 Logic 直写账号）。 */
inline bool FormalModeEnabled() {
    const char *v = std::getenv("GAMEMESH_FORMAL");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
                 std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0);
}

/** 实验能力显式开关：仅当 env=1/true/on 时开启（默认关闭）。 */
inline bool ExperimentalFeatureEnabled(const char *env_name) {
    if (!env_name || !*env_name)
        return false;
    const char *v = std::getenv(env_name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
                 std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0);
}

/** 危险 HTTP 管理接口：默认关闭；GAMEMESH_ENABLE_ADMIN=1 开启。 */
inline bool AdminHttpEnabled() {
    const char *v = std::getenv("GAMEMESH_ENABLE_ADMIN");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

/**
 * 联调危险公网命令（GrantItem/MailDeliver）：仅非 Formal 且显式开启。
 * Formal 时即使环境变量误开也必须拒绝（见 CommandPolicy）。
 */
inline bool AllowUnsafeDebugCommandsEnv() {
    if (FormalModeEnabled())
        return false;
    const char *v = std::getenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
                 std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0);
}

/**
 * 正式模式下是否允许本进程初始化 MySQL 连接池。
 * 仅 gamedb（及紧急 role=all）可直连；gateway/session/gamelogic/world 必须经 GameDB brpc。
 */
inline bool FormalModeAllowsMysql(const std::string &role) {
    if (!FormalModeEnabled())
        return true;
    return role == "gamedb" || role == "all";
}

/** 运行期隐式 DDL（CREATE TABLE）。默认关闭；仅本地显式 GAMEMESH_BOOTSTRAP_DDL=1。Formal 同样必须显式开。 */
inline bool BootstrapDdlEnabled() {
    return ExperimentalFeatureEnabled("GAMEMESH_BOOTSTRAP_DDL");
}

/**
 * Formal 默认强制 ClientHello。GAMEMESH_ALLOW_LEGACY_NO_HELLO=1 为有截止日的兼容开关
 *（文档约定 2026-09-15 后删除）。
 */
inline bool ClientHelloRequired() {
    if (ExperimentalFeatureEnabled("GAMEMESH_ALLOW_LEGACY_NO_HELLO"))
        return false;
    return FormalModeEnabled();
}
