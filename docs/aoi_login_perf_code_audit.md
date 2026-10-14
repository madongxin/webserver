# AOI 与登录洪峰代码审计

基线：本地 `main` `fc5ca6630e71daf99bdf4cd9ac1468684b477a2c`。工作区在此之上有未提交修改。未执行 `git pull`（工作区不干净）。未压正式集群。

## 证据矩阵

| 线索 | 状态 | 证据 |
| --- | --- | --- |
| `VisibleIds` 先扫格子再 `nth_element`，50 是输出上限不是候选扫描上限 | 仍存在 | `runtime/MapRuntime.cpp` `VisibleIds`（约 73 行）。订阅修复没有改掉格子内全量扫描。 |
| `Move` 用移动者自己的可见集当收件人 | 已修复 | `Move` 改为 `RefreshViewers`：MOVE 只发给 `old` 与 `new` 都包含该目标的观察者。`watchers` 是反向索引。 |
| 全图共用一把 `MapRuntime::mu_` | 仍存在 | `Enter` / `Move` / `Leave` 仍持有 `mu_`。本机微基准显示人数上升后耗时上升，但没有做锁拆分。拆锁需要锁顺序证明，这一轮不做。 |
| 60 人测试只数 ENTER 次数 | 已修复 | `test/map_runtime_aoi_test.cpp` 断言玩家 60 不会进入玩家 1 的订阅。`test/aoi_subscription_invariant_test.cpp` 覆盖 AOI-01..08，全部 PASS。 |
| `PushGrouped` 同步 brpc | 仍存在 | `runtime/brpc/GameLogicPush.cpp` 未改。没有新的线上推送剖析，不把同步 RPC 改成异步。 |
| `ConnectedCount` 在全局锁里扫实体 | 仍存在 | `MapRuntime::ConnectedCount` 仍遍历 `entities`。未改调用点。 |
| 登录链路已有 AuthFlow、串行队列、256 离载上限 | 已确认，并补了一层进程内预算 | `GameTcpGateway` 在 `TryBegin` 之前调用 `LoginAdmission`。默认 inflight 256，其中 32 留给 Reconnect。超限返回已有 `ERR_OVERLOADED`。这不是集群级队列。 |
| 每 IP 每 2 秒 80 次连接 | 仍存在 | `GatewayConnGuard::CheckConnectRate`。没有放宽，也没有信任客户端 XFF。 |
| `AuthLoginRateLimit` 按玩家失败次数 | 仍存在 | 它仍是撞库保护，不是全局 QPS。 |
| `gamedb_addrs` 只取第一个 | 部分修复 | `AuthServiceImpl.cpp` `CachedGameDbChannel` 按逗号拆开，`Init` 失败才试下一个。通道一旦建立就钉住，没有按 RPC 健康度再切换。 |
| `scripts/load_test.sh` 只打 HTTP `/health/live` | 仍存在 | 未改该脚本。新增 `scripts/load_game_login_stress.sh`。正式 pid 文件存在时该脚本拒绝执行。LOAD-01..12 未跑。 |

## 复现与修复

修复前，`Enter` / `Move` 把 `VisibleIds(移动者)` 里的人当成收件人。两人叠在同一点时，高 `player_id` 不在低 `player_id` 的最近 50 人里，却仍会收到高 id 的 ENTER。AOI-01 现在断言玩家 1 收不到玩家 60 的 ENTER，并且玩家 1 的快照里没有 60。

修复后每个实例保存：

- `visible_targets[viewer]`：该观察者当前订阅，最多 50，距离相同则 `player_id` 更小者优先。
- `watchers[target]`：谁订阅了这个目标。

位置变化只给仍订阅该目标的观察者发 MOVE。订阅集合变了才发 ENTER 或 LEAVE。进图玩家自己的集合放进 Enter 回包快照，不额外给自己推 ENTER。断线从格子摘掉并给观察者 LEAVE；重连重建订阅。跨实例进图先从旧图摘掉，再进新图。

回滚：恢复 `runtime/MapRuntime.cpp`、`runtime/MapRuntime.h` 和两个测试文件即可。没有改 proto 字段号。
