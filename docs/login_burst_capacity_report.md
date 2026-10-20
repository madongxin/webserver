# 登录洪峰容量报告

机器：4 核，内存约 3.6G。`run/formal/pids` 存在。脚本在该文件存在或目标地址包含 `124.222.244.169` 时直接退出，本轮没有对正式集群或公网发起登录。

因此 LOGIN200-01 .. LOGIN200-06 全部是 NOT_RUN。下面没有吞吐、p95、p99、成功率。不能把 `/health` 或进程内单测当成登录 TPS。

## 本轮代码里有、但没有压出来的保护

- 外层 `LoginAdmission`：默认 in-flight 256，重连预留 32。新登录不能占用预留。拒绝码仍是 `ERR_OVERLOADED`，文案 `login admission`。
- 分阶段 `StagePermit`：Auth、Session、Bind、Enter。环境变量 `GAMEMESH_LOGIN_AUTH_INFLIGHT`、`GAMEMESH_LOGIN_SESSION_INFLIGHT`、`GAMEMESH_LOGIN_BIND_INFLIGHT`、`GAMEMESH_LOGIN_ENTER_INFLIGHT`，默认都是 256。许可只包住对应 RPC。重连的 Bind 使用 `reconnect=true`，可以使用该阶段满额；新登录不能用预留尾部。重连路径上的 Auth 和 Session 没有单独包 StagePermit。
- 拒绝计数：`login_stage_reject_auth/session/bind/enter`。
- 没有新增集群队列，没有 `retry_after` 字段。队列长度没有被放大来掩盖积压。
- GameDB：多个 `gamedb_addrs` 走已有的 `BuildListNamingUrl` + `SessionLoadBalancerName`（多于一个地址时为 `rr`），一条 channel 覆盖整份列表。单地址仍是裸 `host:port`。这不是 Init 成功之后的运行时健康摘除。没有 MySQL 吞吐测量。多个 GameDB 进程不等于 MySQL 吞吐乘以 N。
- NAT：Gateway 连接保护仍是每 IP 80 连接 / 2 秒。没有删除 IP 限流，没有信任客户端 XFF。共享 IP 的分层限流测试未写，记 NOT_RUN。

`login_admission_test` 只覆盖外层预算（max 4、reserve 1 → 3 次登录 + 1 次重连），退出码 0。它不覆盖分阶段预算，也不覆盖真实 Auth/Session/GameDB。

## LOGIN200

| 用例 | 结果 | arrival_rps | admitted_rps | login_success_rps | final_success | p95/p99 | overloaded | retry | queue depth | 瓶颈 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| LOGIN200-01 1000/5s | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 正式 pid 存在，拒绝压测 |
| LOGIN200-02 3000/3s | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 同上 |
| LOGIN200-03 10000/5s | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 同上，且本机 4 核 |
| LOGIN200-04 30000/10s | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 同上 |
| LOGIN200-05 重连比例 | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | `reconnect_ratio` 只写入报告，line-press 不发起重连 |
| LOGIN200-06 共享 NAT | NOT_RUN | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未测 | 未实现分层 NAT 用例 |

## 脚本实际做了什么

`scripts/load_game_login_stress.sh` 在放行前计算 `PRESS_SPACING_MS`：`max(1000/target_rps, ramp_seconds*1000/users)`。`move_hz > 0` 时设置 `PRESS_MOVE_STEPS` 和 `PRESS_MOVE_GAP_MS`。`users > 20` 仍要求 `GAMEMESH_STRESS_ALLOW_LARGE=1`。本轮没有执行该脚本。
