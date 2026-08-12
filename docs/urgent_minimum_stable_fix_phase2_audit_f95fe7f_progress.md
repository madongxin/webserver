# 稳定评估阶段二进度（对照 `GameMesh_最新稳定版本评估_f95fe7f.md`）

基线：阶段一已完成（串行链 + 邮件正式资产）。
本阶段：**故障恢复与 Reactor 稳定性**。不宣称 STABLE PASS。

## 已完成

### P0-1 死亡 Owner 不得被软续租

- `PlacementStore` Lua：READY+lease 过期时，仅当旧 Owner 在健康 CSV（`ARGV[9]`）中才软续租；否则硬 reclaim（升 `owner_epoch`/`route_version`，换健康 Owner）。
- `SessionServiceImpl::ResolveOrCreateMap`：Resolve 前 `Discover(gamelogic)` 刷新健康 Owner。
- `placement_store_test`：存活 Owner 软续租 + 死 Owner 硬 reclaim。
- `map_lease_drill resolve-reclaim` + `kill_logic_drill.sh RESOLVE_RECLAIM=1`：不手工 `MarkRecovering`。

### P0-4 Gateway 断线异步 brpc

- `SessionRpcClient::MarkDisconnectedAsync` / `GatewayAuthClients::UnbindPlayerAsync`（`NewCallback`，不阻塞）。
- `GameTcpGateway` 断线回调：快照 bind/reg → `ForgetBind` → 异步 RPC + flush_bag；done 中不碰 `TcpConnection`。
- `gateway_disconnect_async_test`：64 次不可达目标异步调用必须 &lt;500ms 返回。

## 阶段门禁

| 项 | 状态 |
| --- | --- |
| 死 Owner Resolve 升 epoch | PASS `placement_store_test` |
| 断线风暴不阻塞 | PASS `gateway_disconnect_async_test` (storm_ms=5) |
| RESOLVE_RECLAIM drill | 可选：`RESOLVE_RECLAIM=1 ./scripts/kill_logic_drill.sh` |
| 全量 stable_gate | **未做**（阶段三） |

## 刻意不做

- 实验性 PlacementRecoveryScheduler 仍默认关闭
- Push ACK 连续性 / 门禁 whitespace（阶段三）
