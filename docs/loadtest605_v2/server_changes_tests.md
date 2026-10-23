# 改动和测试

工作区相对 `9ac1a93` 的修改还没提交。`git fetch origin --prune` 之后 `origin/main` 仍是 `9ac1a93`。没有重启，09:41 的进程仍映射已删除的旧二进制。

## 代码

| 文件 | 改动 |
|---|---|
| `game/GameLogic.cpp` | 不在本逻辑上的玩家，全量快照返回失败和 `ERR_AOI_RESYNC_REQUIRED`，不再 `ok=true` 加空实体。测试目标没有 brpc 时，两处取绑定信息的调用不参与编译 |
| `runtime/brpc/GameLogicPush.cpp` | 可靠推送不再在 `PushBatch` 之前分配序号 |
| `runtime/brpc/GatewayPushServer.cpp` | 会话匹配通过后、写入发送缓冲前分配序号 |
| `test/world_snapshot_test.cpp` | 不在图上的玩家必须失败，且实体数为 0 |
| `CMakeLists.txt` | 该测试补上 `PacketTiming.cpp` 和 `EventLoopMetrics.cpp`，否则 `HandleMove` 链不上 |
| `base/PacketTiming.cpp` | `gamemesh_move_hold_total` 的 HELP 从 8KB 改为 1KB |

## 已运行

2026-10-23 11:25 跑测试，11:27 链接最终的 `gateway` 和 `gamelogic`。Debug，`-j2`：

| 目标 | 结果 |
|---|---|
| `gateway`、`gamelogic` | 链接成功。磁盘二进制时间晚于 09:41 的进程 |
| `world_snapshot_test` | 通过。玩家 1 快照 `INSTANCE_LIVE`、实体 1；玩家 99 `ok=false`、`ERR_AOI_RESYNC_REQUIRED`、实体 0 |
| `map_runtime_aoi_test` | 通过。日志里有一条预期的 `ERR_STALE_SEQ` |
| `reactor_integration_test` | 通过。含内核忙时按住最新移动帧，以及一条预期的背压关闭警告 |

## 未运行

600 / 605 人复测没有做。下面档位都是未运行，不能写成「2 秒内完成」。

| 档 | 状态 |
|---|---|
| T0 单人移动 | 未运行 |
| T1 心跳 | 未运行 |
| T2 小规模分线 | 未运行 |
| T3 约 100 人一条线 | 未运行 |
| T4 约 300 | 未运行 |
| T5 605 | 未运行 |
| T6 心跳与移动同时超时 | 未运行 |
| T7 全量快照打到非属主 | 未运行。单元测试覆盖了空实体不再报成功 |
| T8 可靠推送在拒绝时不占序号 | 未运行。代码路径已改，没有压测计数 |

换上这版二进制需要另一次重启。重启会清 Redis 运行时并要求客户端重新登录。这次没有做。
