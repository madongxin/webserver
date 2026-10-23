# 600 人压测请求路径审查

审查时间：2026-10-22 23:20 +0800。工作区 `HEAD` 是 `3ff3d93848962fc39acd2354e2fe35676617e4c0`（2026-10-22 21:48:41 +0800，`Answer move and heartbeat before the client drops the sequence...`）。其后的 AOI 发布线程和安全点异步落库还在工作区，已编译，没有换到正在跑的进程上。

## 版本

| 项 | 值 |
|---|---|
| 分支 | `main`，跟踪 `origin/main`。工作区在 HEAD 之上还有未提交修改 |
| 机器 | 4 核，内存约 3.6G |
| 线上进程启动 | 2026-10-22 21:30:47 +0800。`run/formal/pids`：session 26456/26459，gamedb 26462/26465，world 26560，logic 26563/26566，gateway 26633/26636 |
| 线上 gateway / gamelogic | 启动时映射的 inode 已被本次编译替换，`/proc/<pid>/exe` 显示 `(deleted)`。线上仍是 21:30 那一版，对应 HEAD `3ff3d93`，不含本文后面的 AOI/安全点改动 |
| 磁盘上的新二进制 | `build/test/gateway`、`build/test/gamelogic`，2026-10-22 23:19 编译。未启动 |
| session 二进制 | `build/test/session`，2026-10-21 00:23。`AuthServiceImpl.cpp` / `SessionServiceImpl.cpp` 的修改时间早于该二进制，与 HEAD 中这两份源码一致 |
| world / gamedb 二进制 | 2026-10-16。本次路径不改这两个角色 |
| 配置 | 正式启动脚本写运行目录，广告地址 `10.0.0.2`，游戏口 8081 / 8083。未改超时，未改分线容量 |

未执行 `git pull`。工作区不干净，不能快进。

## 三条请求是否共用同一条链路

心跳在 Gateway 收包线程上当场组 `HeartbeatRsp`，不进 brpc，不进 GameLogic。分线查询只在收包线程里 `TryPost` 到 `RpcOffloadPool`（上限 256），满了立刻 `ERR_OVERLOADED`。移动在收包线程里做路由判断后异步 `Dispatch`，真正改坐标在 GameLogic 的地图邮箱。

三者最后都回到同一条连接的发送缓冲：心跳和移动应答走 `TrySendControl`，AOI MOVE 走每连接的合并槽。慢连接上的 AOI 可以挡住普通写，但控制帧会插到未写完的半帧之后、旧 MOVE 帧之前。

## 移动

| 阶段 | 位置 | 线程 | 阻塞 | 回复 |
|---|---|---|---|---|
| TCP 读完整帧 | `GameTcpGateway::OnMessage` `game/GameTcpGateway.cpp:698` | 该连接的 EventLoop | 水平触发 epoll。同一连接的一批帧里，心跳和 hello 排在移动前面（约 732–737 行） | 解码失败有非法帧计数，不回业务包 |
| 路由 | `FillMapRouteFromRequest` `runtime/brpc/BrpcTransport.cpp:233` | 同一个 EventLoop，在 `PostPlayerRequest`（329 行）里同步调用 | 连接已经在该 `map_instance_id` 且 `owner_epoch != 0` 时直接用 sticky（257 行），不再同步 `GetPlacement`。否则仍同步问 Session | 路由失败回 `map_placement_failed`，带原 seq |
| 转发 | `stub.Dispatch` 异步，`deadline_ms=3000`，`max_retry=0` | brpc 回调 | 收包线程不等待 | 失败或空帧走 `BuildErrorFrame`，移动体会带 `MoveRsp` |
| 入邮箱 | `ExecuteDispatch` `runtime/brpc/GameLogicServiceImpl.cpp:169` | brpc bthread，`CountdownEvent::wait` 让出 pthread | 该地图分片队列已有 8 个移动时立刻 `ERR_OVERLOADED`（274 行），不进 `HandleMove`。`TryPost` 失败同样拒绝 | 错误帧带原 seq 和 `MoveRsp.ok=false` |
| 提交坐标 | `GameLogic::HandleMove` `game/GameLogic.cpp:2653`，`MapRuntime::Move` | 地图邮箱线程，按 `map_instance_id` 串行 | `HandleMove` 自己的计时是 `move/rpc`，不含邮箱排队 | `MoveRsp.ok=true` 表示权威坐标已提交，不表示观察者已收到 AOI |
| AOI | `EmitAoi` → `EnqueueAoi` `game/GameLogic.cpp:367` | 发布线程 `AoiWorkerMain`（238 行）。HEAD `3ff3d93` 在卸载池满时会在邮箱线程里同步排空 | 队列超过 64 个批次时丢掉最旧的纯 MOVE 批次。ENTER/LEAVE 不丢 | 不阻塞 `MoveRsp` |
| 安全点 | `MaybePersistLastSafe(..., false)` `game/GameLogic.cpp:2759` | 节流命中后 `TryPost` 到 `RpcOffloadPool`。HEAD `3ff3d93` 在邮箱线程里同步 `SaveLastSafePosition`（`db/BrpcGameDbRepository.cpp:534`，`done=nullptr`） | 池满则留下 dirty，下次移动再试，不在邮箱线程里写库 | 落库失败不改变已经提交的 `MoveRsp` |
| 写回客户端 | `OnDispatchDone` → `TcpReplySink::SendFrame` | 原连接的 EventLoop | 移动应答 `TrySendControl(..., true)`，上限 256KB。仍失败则 `reply_soft_drop`，连接不关 | 入队不等于内核已写完。`move_flush` 才是交给 write 路径；`move_hold` 是停在应用缓冲 |

`PushBatch` 仍是同步 brpc，超时 2000ms（`GatewayPushClient.cpp:57` 和 `119`）。它发生在 AOI 发布线程，不在 `HandleMove` 返回之前。

## 心跳

`game/GameTcpGateway.cpp:882`。收包线程组包，`MakeRoomForControl` 丢掉尚未写入内核的 AOI MOVE，再 `TrySendControl(out, true)`（911 行）。失败只记 `heartbeat_soft_drop`，不关连接。不查 Redis，不做 AOI，不等待 `PushBatch`。

心跳超过 12 秒仍无回包时，处理函数本身不是原因：2026-10-21 15:10–15:40Z 两个 Gateway 的 `heartbeat/handler` 10 秒窗口最大值是 11ms 和 9ms。收包线程被别的同步调用占住时，这个处理函数根本还没开始。

## 分线查询

`GameTcpGateway.cpp:1343` 调用 `BeginOrchestrateGatewayQueryMapLines`（`GatewayEnterMapOrchestrator.cpp:694`）。收包线程只负责 `TryPost`。池满返回 false，Gateway 回 `ERR_OVERLOADED`。真正查询在卸载池，完成后回到玩家串行队列再 `SendFrame`。

## 序号和连接代际

`MoveRsp.seq` 来自请求。`BuildErrorFrame` 在有 `move` 体时写入 `MoveRsp.error_code`，避免只有顶层错误码、客户端对不上序号。Gateway 推送按 `session_id` 找连接，并核对 player、fence、generation（`GatewayPushServer.cpp`）。代际不一致计为 rejected，不投给新连接。

客户端在 2 秒后丢掉 pending 序号。服务端没有“迟到回包”专用计数。晚到的成功帧仍会写入原连接；客户端对不上序号时，这次写入对客户端无效。这一点需要客户端 raw frame 才能和 `reply_soft_drop` 分开。

## 已排除 / 已确认 / 未知

| 假设 | 结论 |
|---|---|
| 移动校验拒绝导致 57% 超时 | 已排除。现场拒绝数是 0。`MoveRsp` 回来的是成功 |
| `HandleMove` 本身要数秒 | 已排除。Logic `move/rpc` 窗口最大 177ms（gl-1）和 126ms（gl-0） |
| 心跳处理函数要 12 秒 | 已排除。窗口最大 11ms / 9ms |
| 收包线程上的同步 `GetPlacement` | 已确认是 21:30 之前那一版的心跳/移动互堵原因。`3ff3d93` 对已进图移动跳过。正在跑的 21:30 进程包含这项 |
| 地图邮箱用 `condition_variable` 占死 brpc pthread | 已确认是旧路径。`3ff3d93` 改为 `CountdownEvent`。正在跑的进程包含这项 |
| 邮箱排到客户端丢序号之后才回 | 已确认。`3ff3d93` 在队列深度 ≥ 8 时回 `ERR_OVERLOADED`。正在跑的进程包含这项 |
| 卸载池满时在邮箱线程同步 `PushBatch` | 已确认存在于正在跑的 `3ff3d93`。本次工作区已改成独立发布线程。未换进程，所以线上仍是旧行为 |
| 节流安全点在邮箱线程同步写 GameDB | 已确认存在于正在跑的 `3ff3d93`。本次工作区改为卸载池。未换进程 |
| 客户端把迟到的成功包匹配掉 | 未知。服务端没有 raw receive。不能把 write 成功当成客户端已匹配 |
| 本次改动把 600 人及时完成率提到 99% | 未知。T0–T8 未执行，新二进制未上线 |
