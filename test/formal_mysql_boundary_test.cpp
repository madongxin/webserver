/**
 * 阶段 6：正式模式 MySQL 边界 — ForbidInit 阻止建池；FormalModeAllowsMysql 仅 gamedb/all。
 */
#include "ConnectionPool.h"
#include "FormalMode.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

int main() {
    ConnectionPool::ForbidInit("formal_mysql_boundary_test");
    if (!ConnectionPool::IsForbidden())
        return Fail("IsForbidden");
    if (ConnectionPool::getconnectionPool()->isInitialized())
        return Fail("pool must not init after ForbidInit");

    ::setenv("GAMEMESH_FORMAL", "1", 1);
    if (FormalModeAllowsMysql("gamelogic") || FormalModeAllowsMysql("world") ||
        FormalModeAllowsMysql("gateway") || FormalModeAllowsMysql("session"))
        return Fail("non-gamedb roles must be denied under FORMAL");
    if (!FormalModeAllowsMysql("gamedb") || !FormalModeAllowsMysql("all"))
        return Fail("gamedb/all must allow under FORMAL");

    ::unsetenv("GAMEMESH_FORMAL");
    if (!FormalModeAllowsMysql("gamelogic"))
        return Fail("compat mode allows mysql");

    std::printf("PASS formal_mysql_boundary_test\n");
    return 0;
}
