# 改动与测试

## 已经在 21:30 进程里（`3ff3d93`）

这些不是本次工作区的新 diff，列在这里是因为它们就是针对这次 600 人超时的上一轮修复，而且正在跑。

- `runtime/brpc/BrpcTransport.cpp`：已进图的移动不再同步 `GetPlacement`。
- `runtime/brpc/GameLogicServiceImpl.cpp`：邮箱等待改为 bthread `CountdownEvent`；移动队列深度 ≥ 8 回 `ERR_OVERLOADED`。
- `runtime/brpc/BrpcTransport.cpp`：错误帧带 `MoveRsp` 体和原 seq。
- `tcp/TcpConnection.cpp`、`game/GameTcpGateway.cpp`、`runtime/TcpReplySink.cpp`：心跳和移动应答可写到 256KB；AOI 最新帧在内核未发送超过 8KB 时停住。
- `runtime/MapRuntime.cpp`：限速内的步伐按请求的 XZ 接受。

## 本次工作区，已编译，未换进程

线上 `gateway` 26633/26636 和 `gamelogic` 26563/26566 的 `/proc/pid/exe` 指向已删除的旧 inode。磁盘上 23:19 的新文件没有被这些进程执行。

| 文件 | 改动 |
|---|---|
| `game/GameLogic.cpp` | AOI 由单独线程发布。邮箱线程不再在 `TryPost` 失败时同步 `DrainAoiQueue`。队列超过 64 批时丢掉最旧的纯 MOVE。非强制安全点改走卸载池 |
| `runtime/AoiPublishQueue.h` | `ShedAoiMoveBatches`：只丢 op=2 的批次 |
| `game/GameLogic.h` | `StopAoiPublisher` |
| `apps/ServerBootstrap.cpp` | 退出时先停 AOI 线程。仅 `gamelogic` 角色再停卸载池，避免和 Gateway 自己的停止重复拆掉登录池 |
| `runtime/brpc/GameLogicPush.cpp` | 每次同步 `PushBatch` 记耗时 |
| `runtime/brpc/GameLogicServiceImpl.cpp` | 移动在 `HandleMove` 之前被拒绝时增加 `move_overloaded` |
| `runtime/OpsMetrics.h` / `.cpp` | 上表用到的计数和 `pushbatch_last_ms` |
| `test/map_runtime_aoi_test.cpp` | 队列丢弃行为 |

没有改客户端超时，没有放宽 AOI 200 人上限，没有关认证、序号或分线容量，没有按机器人身份绕过移动校验。

## 命令和结果

```bash
cmake --build build -j2 --target map_runtime_aoi_test gateway gamelogic
./build/test/map_runtime_aoi_test
./build/test/reactor_integration_test
```

2026-10-22 23:19：

- `map_runtime_aoi_test`：`OK map_runtime_aoi_test`，退出码 0。日志里有一条既有的 `ERR_STALE_SEQ` 警告，测试仍通过。
- `reactor_integration_test`：`reactor_integration_test PASS`，退出码 0。覆盖部分写停住、EAGAIN、慢对端、最新移动帧不写进内核、控制帧插到停住的移动帧之前。
- T0–T8：未执行。
- 10～50 人在线：未执行。

## 回滚

不重启则线上行为不变。若新二进制已经换上：

1. 用 `3ff3d93` 重新编译 `gateway` 和 `gamelogic`。
2. 再按正式流程停、起。安全点会回到邮箱线程同步写库，卸载池满时 AOI 也会回到邮箱线程同步 `PushBatch`。
3. 不要用加大客户端超时或加大 socket 缓冲当作回滚。
