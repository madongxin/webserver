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

/** 危险 HTTP 管理接口：默认关闭；GAMEMESH_ENABLE_ADMIN=1 开启。 */
inline bool AdminHttpEnabled() {
    const char *v = std::getenv("GAMEMESH_ENABLE_ADMIN");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
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
