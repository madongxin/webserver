# 服务器 Move / Session / AOI 核查

基线分支 `main`，HEAD `b63c926`（`Drop the parsed-log bytecode cache.`）。工作区有未提交改动。本文件只记录当前源码，不把历史压测写成通过。正式集群没有为这次改动重启。

服务器不实现 Idle / LocalWander / AoiBoundaryShuttle / HotspotGather / BidirectionalPatrol。这五种策略由 Unity 机器人发真实 Login、EnterMap、Move。服务端没有机器人旁路。

## 源码核查矩阵

| 项目 | 位置 | 现有行为 | 是否改 | 风险 |
| --- | --- | --- | --- | --- |
| Gateway 收包 | `GameTcpGateway` 帧分发 | 绑定连接上的 Move 用 Gateway 权威 player/session，不信客户端自报身份 | 否 | 低 |
| Move 权威 | `GameLogic::HandleMove` → `MapRuntime::Move` | 校验进图、epoch、死亡、坐标、速度后改权威位置，再填 `MoveRsp` | 否 | 低 |
| 速度 | `MapRuntime::Move` | 默认 `move_speed` 10 m/s。时间用 `client_time_ms` 间隔，没有则用收包间隔。不超过 `speed * dt` 的请求原样接受，含导航格不可走的 XZ | 否 | 超速仍缩短并可能 `ERR_MOVE_TOO_FAST` |
| 地面 | 同上 | 几乎无水平位移时最多下降 0.4 m，否则水平距离 + 0.5 m。贴着 Y 盒底抬回出生 Y | 否 | 客户端贴地 Y 可能被抬高几厘米并记 OK 修正 |
| AOI 发布 | `SampleMoveEvents`，`kAoiMoveSampleMs = 200` | 同一收件人、同一实体 200 ms 内只留更大 `state_seq` 的 MOVE。ENTER/LEAVE 不抽样 | 否 | 观察者看到的别人最高约 5 Hz，与自己的 MoveRsp 频率不是一回事 |
| 出站 | `FlushMoveSlot` → `TcpConnection::TrySendLatestMove` | 每连接先丢掉缓冲里的旧移动帧，再留一帧最新位置。内核未发送字节超过 8 KB 时不 write，只停放。非移动数据已超过 64 KB 则本轮不入队，200 ms 后再试。不关连接 | 是 | 回包仍插在停放帧前面。未重启前线上仍是旧的 32 KB 拒绝逻辑 |
| 分线 | `PlacementStore` Lua，`n >= hard` 返回 `ERR_MAP_LINE_FULL` | 1002 是 LINE：soft 100、hard 400、最多 8 线。`line_no=0` 在未满软顶的线里选人少的，必要时开新线 | 否 | 单线装不下 600 人 |
| 可见上限 | `MapRuntime::VisibleIds` | 自己不进候选。`aoi_max_visible` 默认 200，1002 显式 200。距离平方更小优先，平局 `player_id` 更小优先 | 否 | 人堆在一格时观察者最多看见 200 个别人 |
| 监控 | 各进程 `/metrics`，观察页 GameDB `8094/monitor` | 已有收发、Move 合并、AOI、Session ONLINE。本轮只加停放计数 | 是 | 没有 P95/P99 直方图 |

## Move 调用链

`Gateway 解码 MoveReq` → 连接绑定覆盖身份 → `GameLogic.Dispatch` → 地图邮箱上的 `HandleMove`：

1. `player_id`、`map_instance_id`、坐标缺失：`ERR_INVALID_ARG`。
2. 绑定的 `map_instance_id` 与请求不一致：`ERR_MAP_MISMATCH`。不会写入旧实例。
3. `owner_epoch` 过期：`ERR_STALE_EPOCH`。
4. 人不在该图：`ERR_MAP_NOT_LOADED`。
5. 死亡或复活中：`ERR_PLAYER_DEAD`。
6. `MapRuntime::Move`：不在图 `ERR_NOT_ON_MAP`，已断线 `ERR_DISCONNECTED`，`client_seq` 不大于上次 `ERR_STALE_SEQ`，出界 `ERR_OUT_OF_BOUNDS`，缩短后仍不可走 `ERR_MOVE_TOO_FAST`。
7. 接受后 `state_seq` 加一，`EmitAoi` 产生 op=2。`MoveRsp.ok=true`，坐标、yaw、`state_seq`、`server_time_ms` 是权威值。

拒绝时只要人还在这张图，回包仍带当前权威坐标，`ok=false`。客户端必须采用这份坐标，不能用本地飞远的点重发。

`MoveRsp.ok=true` 只表示权威位置已经提交。它不表示其他客户端已经收到 AOI。AOI 还要经过 200 ms 合并，再进入 Gateway 的每连接一帧发送。

## 频率

服务端没有 2/5/10 Hz 的包速率上限，也不为压测放开限速。约束是位移：

- 计划 5 Hz、`move_speed=10` 时，一步大约不超过 `10 * dt` 米。`dt` 来自相邻 `client_time_ms`。
- 10 Hz 同样允许，只要每步距离不超过 `speed * dt`。
- 超过 `speed * dt * 1.25 + 0.5` 才会沿方向缩短；缩短后的点不在可走格上，才返回 `ERR_MOVE_TOO_FAST`。
- 观察者侧 AOI MOVE 被合并到约 200 ms 一帧，所以别人看见的更新不会高于大约 5 Hz，即使本人 MoveRsp 是 10 Hz。

## 地图与实例

权威键是 `map_instance_id`，不是模板号，也不是 `line_no`。同模板不同线是不同实例，AOI 不跨实例。

1002（`config/maps/1002.grid.json` + manifest）：

| 项 | 值 |
| --- | --- |
| 种类 | LINE |
| soft / hard / max_lines | 100 / 400 / 8 |
| AOI 格子 | 32 m，`aoi_view_radius_cells=1` |
| 导航格 | 8 m，只影响超速后的可走收束 |
| 可见上限 | 200 个其他实体，不含自己 |
| 范围 | X/Z `[-1992, 1992]`，Y `[-38, 536]` |
| 出生点 | `(179.3, -18.687, 324.2)` |

单线满员是 `ERR_MAP_LINE_FULL`，不会静默换线。`line_no=0` 才会系统选线。因此：

- 100 / 200 / 201 / 250 / 400 人可以落在同一条 1002 线上（400 等于 hard，再进一人被拒）。
- 600 人不能塞进一条线。系统选线会拆到多条线，AOI 只在各自 `map_instance_id` 内。
- 8 线 × 400 = 3200。3000 人在容量上有可能，但是否均衡要看当时的选线，不能写成单线承载。10000 人超过 1002 总 hard，记 `CAPACITY_BLOCKED`。

没有 `PickLoadTestLine`。机器人和真人走同一条 EnterMap / ReserveLine。

## 已确认并修的出站缺陷

旧逻辑在「应用缓冲 + 内核未发送字节 > 32 KB」时直接不放入最新移动帧。内核里留下旧坐标，MoveRsp 排在后面。客户端要读完旧数据才走下一步，机器人会越走越慢，最后停下。权威位置当时仍在更新。

现在每连接只保留一帧最新 MOVE。内核未发送超过 8 KB 时这一帧停在应用缓冲，不写进内核；下一轮先丢掉再换成更新的位置。回包和心跳仍插到这帧前面。非移动数据已经超过 64 KB 才放弃本轮入队。不因为发送积压关闭在图连接。

回归：`reactor_integration_test` 的 `TestLatestMoveHeldWhileKernelBusy`、`TestLatestMoveSendsWhenIdle`。

## 验收对照

| 项 | 结论 |
| --- | --- |
| SRV-01 | 未进图、死掉、epoch/实例不匹配的 Move 被拒。代码如此，本轮未再跑双人 E2E |
| SRV-02 | `client_seq` 不递增返回 `ERR_STALE_SEQ`。旧 session generation 在 Gateway/Session 绑定路径拒绝，不在 Move 里另开机器人规则 |
| SRV-03 | 没有 Bot 身份开关 |
| SRV-04 | 无 Hz 上限；速度见上。拒绝码是现有 `ERR_*` |
| SRV-05 | MoveRsp 成功 = 权威提交。AOI 下发看 `gamemesh_aoi_move_events_total` 与 Gateway `move_flush` / `move_hold` |
| SRV-06 / 07 | 同实例才互相可见。本轮未跑 1 真人 + 1 Bot |
| SRV-08 | 格子用 `WorldToAoiCell`，原点可以不是 0。1002 的 X/Z 可以为负。本轮未新跑边界用例 |
| SRV-09 | 可见人数不含自己，上限 200。单线超过 400 人被 `ERR_MAP_LINE_FULL` |
| SRV-10 | 差集 ENTER/LEAVE 与断线 LEAVE 是既有逻辑。本轮未重跑 aoi200 套件 |
| SRV-16 | 没有放宽真人速度、容量或认证 |
