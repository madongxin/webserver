# 登录洪峰容量计划

## 链路

客户端 TCP → Gateway Hello / Login → Auth（GameDB 查账号）→ Session Acquire → GameLogic Bind → EnterMap。Reconnect 走同一条 Gateway 流程槽，并保留 Session fence。失败补偿仍在 `GatewayLoginOrchestrator`，本轮没有删。

## 已有保护（保留）

- 单连接同时只允许一个 Login 或 Reconnect（`GatewayAuthFlow`）。
- `PlayerSerialQueue` 与 `RpcOffloadPool`（待处理上限 256）满了返回 `ERR_OVERLOADED`。
- 每来源 IP 每 2 秒最多 80 个新连接（`GAMEMESH_CONNECT_RATE_MAX` 可调）。不读取客户端自己填的 XFF。
- `AuthLoginRateLimit` 只锁失败的 `player_id`，防止撞库。

## 本轮新增

`LoginAdmission`（`runtime/LoginAdmission.cpp`）是 **单个 Gateway 进程** 的在途预算：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `GAMEMESH_LOGIN_INFLIGHT_MAX` | 256 | 该进程同时进行中的 Login+Reconnect |
| `GAMEMESH_LOGIN_RECONNECT_RESERVE` | 32 | 其中留给 Reconnect 的名额。新 Login 用剩下的 |

超限返回已有 `ERR_OVERLOADED`，文案 `login admission`。没有新增 proto 字段，也没有 `retry_after`。客户端把 `ERR_OVERLOADED` 当作可稍后重试。

回滚：去掉 `GameTcpGateway.cpp` 里 `LoginAdmission` 的获取/释放，或把 `GAMEMESH_LOGIN_INFLIGHT_MAX` 调回 256。不要把它设成 0，0 会拒绝全部登录。

## GameDB

`CachedGameDbChannel` 会依次尝试 `gamedb_addrs` 里每个地址，直到 `Channel::Init` 成功。成功后钉住该地址。这不是运行中的健康切换，也不是按 QPS 分摊。Auth 查库热点仍可能在一台 GameDB 上。

## 不能从配置里变出来的容量

1002 的 soft 100 / hard 400 / max_lines 8 是进图配额，不是测得的 CCU。`spawn_scatter_radius` 仍按模板配置，没有把所有图改成同一个半径。

## 压测边界

`scripts/load_game_login_stress.sh` 默认不执行。`run/formal/pids` 存在，或地址里出现公网 EIP，直接拒绝。`scripts/load_test.sh` 仍只测 HTTP 健康检查，它的 QPS 不能写成登录 TPS。

LOAD-03 到 LOAD-12（3 千至 3 万连接、依赖故障、重连风暴）需要隔离的多进程环境和测试账号。这台机器 4 核、约 3.6GB 内存，并且正式集群正在跑，这些场景标 NOT_RUN。
