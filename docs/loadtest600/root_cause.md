# 根因

现场数字来自客户端面板和 Prometheus `2026-10-21T15:10:00Z`–`15:40:00Z`（北京时间 23:10–23:40）。那一版二进制还没有 `3ff3d93`。下面每一条都标明现在跑的进程有没有包含修复。

客户端当时的现象：约 605 人，两个 Gateway；约 340 次移动/秒发出，约 150 次在 2 秒内完成；`MoveRsp` 约 57% 超过 2 秒；拒绝数 0；心跳可以超过 12 秒，TCP 仍在。

## 1. 收包线程同步查路由

- 症状：心跳和移动一起超时，人不断线。心跳处理窗口最大只有 11ms / 9ms，说明函数没跑到，或者回包排在后面。
- 代码：`PostPlayerRequest` 在 EventLoop 上调用 `FillMapRouteFromRequest`。旧代码对每个带地图 id 的移动同步 `SessionRpcClient::GetPlacement`。
- 测量：Gateway `move/handler` 窗口最大 122ms（gw-0）和 120ms（gw-1）。这个数字包含同步路由，不包含整段 `MoveRsp` RTT。两个 Gateway 收包约 190 帧/秒和 180 帧/秒。
- 修复：已进图且 epoch 非 0 的移动用连接上的 sticky。在 `3ff3d93`，21:30 的进程里已经有。
- 测试：没有单独的路由单测。reactor 测试不覆盖 `GetPlacement`。
- 压测对比：修复之后的 600 人未跑。
- 回滚：恢复 `FillMapRouteFromRequest` 里对移动的 sticky 短路。不要靠加长客户端超时。

## 2. 地图邮箱把 brpc 工作线程占死，队列超过客户端时限才回

- 症状：Logic `move/rpc` 最大 177ms / 126ms，但客户端 2 秒等不到。拒绝数是 0，所以不是速度校验。
- 代码：`ExecuteDispatch` 以前在 brpc 线程上 `condition_variable::wait`。邮箱深度当时窗口最大 gl-1 为 134、gl-0 为 122。
- 测量：Logic CPU 没有打满（先前同一窗口约 0.6 核）。排队和线程占住比业务计算更像原因。
- 修复：`bthread::CountdownEvent` 让出 pthread。移动队列深度 ≥ 8 时立刻 `ERR_OVERLOADED`，`MoveRsp` 带原 seq。在 `3ff3d93` 和 21:30 进程里。这是忙时拒绝，不是坐标拒绝。
- 测试：未加邮箱单测。编译通过。
- 压测对比：未跑。上线后应看到 `move_overloaded` 增加、2 秒无回包减少。若拒绝暴涨而完成率仍低，说明 8 的水位或地图分片不够，不是校验失败。
- 回滚：去掉深度判断会回到“排队直到 3 秒 Dispatch 超时”。不要把 Dispatch 超时改成 10 秒。

## 3. 卸载池满时，邮箱线程同步做 PushBatch

- 症状：`MoveRsp` 要等 AOI 推送。`PushBatch` 超时配置是 2000ms，和客户端 2 秒重合。
- 代码：`3ff3d93` 的 `EnqueueAoi` 在 `RpcOffloadPool::TryPost` 失败时直接 `DrainAoiQueue()`。`PushGrouped` 调用 `GatewayPushClient::PushBatch`，`done=nullptr`。`HandleMove` 在 `EmitAoi` 之后才填成功应答。
- 测量：这一条没有单独的 PushBatch 耗时，因为旧进程没有 `pushbatch_*`。同一窗口 `gamemesh_aoi_move_events_total` 速率峰值是 gl-0 16229/秒、gl-1 15927/秒（按观察者计）。`move_hold` 峰值 1262/秒和 1273/秒，`move_flush` 峰值 301/秒和 277/秒。这是 Gateway 发送跟不上，不能单独证明邮箱线程正在同步等 `PushBatch`。
- 修复：工作区改为一个 AOI 发布线程。邮箱只入队。队列超过 64 批时从最旧的纯 MOVE 批次丢起，ENTER/LEAVE 保留。`PushBatch` 仍同步，但只堵发布线程。新二进制未换上，线上仍会在池满时同步排空。
- 测试：`map_runtime_aoi_test` 断言丢 3 个纯 MOVE、留下 op=3。通过。没有 600 人对比。
- 回滚：删掉发布线程，恢复 `TryPost` 失败时的原地 `DrainAoiQueue`。

## 4. 节流安全点在 MoveRsp 之前同步写 GameDB

- 症状：同一条线的移动串行。人每走出约 4 米或每 5 秒，下一次移动要等一次同步 `SaveLastSafePosition`。
- 代码：`HandleMove` 在成功路径调用 `MaybePersistLastSafe(..., false)`，旧实现直接 `FlushLastSafe`。`BrpcGameDbRepository::SaveLastSafePosition` 使用同步 stub。
- 测量：没有按“安全点耗时”拆开的指标。权威坐标在这次 RPC 之前已经写入 `MapRuntime`。
- 修复：工作区把非强制落库 `TryPost` 到卸载池。池满保持 dirty，下次再试。断线和下线的 `force=true` 仍同步写。新二进制未上线。
- 测试：未做 GameDB 集成。编译通过。
- 回滚：`MaybePersistLastSafe` 的非强制分支改回直接 `FlushLastSafe`。

## 5. 发送缓冲里的 AOI 把应答留在后面

- 症状：监控上的 reply queue 可以很短，客户端仍超时。帧已经挂在连接缓冲或内核未发送队列里。
- 代码：部分写前缀、最新移动帧在内核未发送 > 8KB 时停住并不武装 `EPOLLOUT`、控制帧插到停住的移动帧之前。心跳和移动应答上限 256KB。
- 测量：发送待写窗口最大 gw-0 52480 字节、gw-1 28322 字节。没有再堆到数百 KB。`move_hold` 远高于 `move_flush`。
- 修复：已在 21:30 进程中。本次没有再放宽 socket buffer，也没有把超时改成 30 秒。
- 测试：`reactor_integration_test` 通过，含 EAGAIN 时停住最新移动帧、控制帧插到前面。
- 回滚：不要恢复“缓冲超限就继续追加移动帧”。

## 待验证

- 客户端 2 秒后清掉的序号，晚到的成功 `MoveRsp` 有多少写进了 socket。服务端写成功不能证明匹配成功。
- 21:30 进程上再跑 600 人，超时还剩多少。那一版已有根因 1、2、5，没有根因 3、4 的修复。
- 换上本次二进制之后，`pushbatch_slow`、`aoi_move_shed`、`safe_pos_deferred`、`move_overloaded` 和客户端 2 秒完成率。
- 分线查询超时有多少是卸载池排队。代码路径已离开收包线程，现场没有单独的 query_lines 窗口最大。
