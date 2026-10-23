# 回退

线上现在跑的仍是 12:14 的进程，映射的是被覆盖前的 inode。磁盘上 13:21 的 `build/test/gateway` 和 13:22 的 `build/test/gamelogic` 含这次改动，但还没被拉起。

不把这版换上去，就不需要回退运行中的进程。客户端保持现状即可。

若已经重启到这版，再回到 `86391f0` 的二进制：

1. 用当时留下的 `gateway` / `gamelogic` 覆盖 `build/test/`，或在干净的 `86391f0` 上重新编译这两个目标。
2. `./scripts/stop_formal.sh` 然后 `GAMEMESH_ADVERTISE_HOST=10.0.0.2 ./scripts/start_formal.sh`。
3. 重启会清掉 Redis 里 `gamemesh:dev:` 的运行时，客户端必须重新登录。

源码回退时一起撤掉这些文件里的本次改动：`TcpReplySink.cpp`、`GameTcpGateway.cpp`、`GatewayConnRegistry.h`、`GatewayConnRegistry.cpp`、`GameLogic.cpp`、`MapRuntime.h`、`OpsMetrics.h`、`OpsMetrics.cpp`、`TcpConnection.h`、`TcpConnection.cpp`，以及 `test/reactor_integration_test.cpp`、`test/world_snapshot_test.cpp`、`test/gateway_conn_race_test.cpp`、`test/auth_session_boundary_test.cpp`。

回退后可靠推送会重新把「已投递到 EventLoop」当成成功，快照默认上限回到 128，暂存移动帧不再有 200ms 唤醒。认证、Session fence 和地图软/硬上限都没有在这次改动里动过。
