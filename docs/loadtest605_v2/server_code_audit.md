# 605 人压测代码审计

审计对象是 2026-10-23 的工作区，基线提交 `9ac1a93`（`origin/main` 与之相同）。正式集群是 09:41 拉起的进程，`/proc/<pid>/exe` 指向已删除的旧二进制。磁盘上 11:27 的 `build/test/gateway` 和 `build/test/gamelogic` 包含本文的修改，还没有换上去。没有重启。

## 这次压测实际走的路径

客户端连 VIP 的 8081 / 8083。Gateway 在连接自己的 EventLoop 上处理心跳，不进 GameLogic。移动经 `PostPlayerRequest` 进地图邮箱，`HandleMove` 提交权威坐标后把 AOI 放进独立发布线程。`PushBatch` 仍是同步 brpc，超时 2 秒，但调用方是 AOI 线程，不在 `HandleMove` 返回 `MoveRsp` 之前。

`MoveRsp.ok=true` 只表示这台逻辑已经收下坐标。它不表示周围的人已经收到 AOI。

## 已在 9ac1a93 落地、09:41 进程没有的部分

- 进图不再在地图邮箱上同步写安全点。非强制安全点交给卸载池。
- 最新移动帧在内核未发送超过 1KB 时留在应用缓冲，不继续把 AOI 写进内核挡在 `MoveRsp` 前面。
- AOI 发布队列上限 64 批，超限只丢纯 MOVE。ENTER / LEAVE 保留。

09:41 的进程仍是进图同步写库、内核移动预算 8KB 的那一版。下面的窗口数据描述的是那一版。

## 本轮确认并改掉的两处

1. 玩家不在这台 GameLogic 的 `MapRuntime` 里时，`BuildFullStateSnapshot` 原来仍 `ok=true`，`aoi_entities` 为空。客户端把空列表当成全量替换，会把视野里的人删掉。现在这条分支返回 `ok=false`、`ERR_AOI_RESYNC_REQUIRED`，并清掉实体列表。人确实在这张图上、邻域为空时，仍可以 `ok=true` 且 `recovery_reason` 表示视野为空。`world_snapshot_test` 里玩家 1 和邻居 2 仍是 `INSTANCE_LIVE` 且至少 1 个实体。

2. 可靠推送原来在 `PushGrouped` 里先分配 `server_seq`，再调用可能被 Gateway 拒绝的 `PushBatch`。拒绝发生在会话还没绑上、fence 或代际不匹配时，序号已经进了回放，客户端看不到这一帧，随后 `PushAck` 报缺口。现在序号改在 `GatewayPushServer::PushBatch` 里，会话、玩家、网关、fence、代际都通过之后，写进发送缓冲之前分配。不可靠的移动帧仍不占 `server_seq`。

`FindBySession` 成功后、`SendBySession` 之前连接被拆掉，仍可能留下一个序号。这是断线竞态，不是拒绝路径。

## 没有改的部分

- 没有把 `PushBatch` 再改成异步。09:44 窗口里它的调用已经不在 `HandleMove` 里面，移动阶段最大值是几十毫秒。
- 没有放宽 8KB 普通发送上限，没有改 3000ms Dispatch 超时，没有关认证，没有改地图 100 软上限或 400 硬上限。
- 没有加 `player_id` 标签，没有做 P50 / P95 / P99。
