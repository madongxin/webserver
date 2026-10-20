# 压测优化：Session CPU / 心跳 / 慢请求不踢连接（2026-10-16 23:xx）

## 本轮现象

| 现象 | 证据 |
|------|------|
| 在线 177/605、CLIENT_TIMEOUT×440 | 心跳/登录在 4s 客户端超时内未回 |
| Session sess-0/1 CPU ~100% | 监控实例表；绑定玩家 — 仍满核 |
| Bound/ONLINE 卡 600，gw-1 TCP 先掉 | TCP 已断但 MarkDisconnected 被 Session 拖死 |
| 出站帧 ~10× 入站 | AOI/Move 广播过重 |

客户端 `get_persistentDataPath` 主线程错误属 Unity 压测机器人问题，服务端无法修。

## 根因

1. **Session `/metrics` → `OnlinePlayerCount`**：每次刮取对 ONLINE 集 **SMEMBERS + 逐成员 HGET**，双 Session × 高频 Prometheus → Redis/CPU 打满。
2. **Session 安装 `MapLineSync`**：`MarkDisconnected`→`ParkForGrace`→`FireLineStatus`→对全体 occupants `PushBatch`，断线风暴时 Session 自燃，拖死断线减人数。
3. **心跳/回包 Hard `Send`**：AOI 积压时 `CloseForBackpressure` 踢 TCP，客户端被迫重连 → `login_after_reconnect_closed` 超时。

## 已落地

1. **心跳**：Gateway 本连接同步 `TrySendUrgent`；丢帧记 `heartbeat_soft_drop`，**不踢连接**。
2. **TcpReplySink**：一律 `TrySendUrgent`，禁止 Hard Send 踢连接；`reply_soft_drop` 计数。
3. **Session**：去掉 `MapLineSync::Install`（分线推送仅 GameLogic）；`OnlinePlayerCount` 改 SCARD + 2s 缓存 + 抽样对账。
4. **occ 对账**：每 ~15s 跑一次，降低与 MarkDisconnected 抢 Redis。
5. **AOI**：Move 合并间隔 100ms→**200ms**；`MapLineSync` 默认 5s→**8s**。
6. **指标**：`gamemesh_heartbeat_soft_drop_total`、`reply_soft_drop_total`、`line_occupancy_total`、`ghost_occupancy`、`aoi_push_backlog`。

## 客户端建议对照

| 建议 | 状态 |
|------|------|
| 1 心跳优先独立 | ✅ Gateway 本地路径 + Urgent |
| 2 Reconnect 独立限流 + retryable | ✅ 已有 LoginAdmission 预留 + `kErrOverloaded` retryable |
| 3 慢不踢连接 | ✅ Reply/心跳不再 Hard Close |
| 4 AOI 降载 | ✅ 200ms 合并 + 8s 分线广播 |
| 5 断线减人数 | ✅ ParkForGrace；Session 不再被 MapLineSync 堵死 |
| 6 EnterMap 热路径 | 已有 line_no=0 系统选线 Lua；未改协议 |
| 7 Gateway 写缓冲 | ✅ TrySend/Urgent 水位 |
| 8 观测 | ✅ 上述 Prometheus 指标 |

## 验证

再跑 605：Session CPU 应远低于单核满载；`ghost_occupancy` 在断线后快速下降；`CLIENT_TIMEOUT` 大幅减少；Bound 与 TCP 曲线同步下降。
