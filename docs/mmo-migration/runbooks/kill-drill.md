# Runbook：杀服演练（Kill Drill）

适用拓扑：`session` / `gamedb` / `world` / `gamelogic` / `gateway`（或等价 `server <role>`）。

本地一键拓扑：`./scripts/run_midterm_local.sh`。

## 通用准备

1. 确认 `config/gateway.cnf` 静态地址或 `etcd_endpoints` 可用。
2. 观察 `/metrics` 与各进程日志（`./log/角色二进制.log`）。
3. 客户端保持一条已 Login 的 Game TCP 连接（或随时可重连）。

## Gateway

| 项 | 说明 |
|----|------|
| 操作 | `kill -TERM <gateway_pid>` |
| 预期 | 玩家 TCP 断开；Logic/World/Session 仍存活 |
| 恢复 | 重启 `gateway`（或 `server gateway`）；客户端 Reconnect / 再 Login |
| 注意 | 会话状态在 Session/Redis；Gateway 无状态转发 |

## GameLogic

| 项 | 说明 |
|----|------|
| 操作 | `kill` gamelogic |
| 预期 | 进图/场景/MapPing 失败；邮件（World）仍可用 |
| 恢复 | 重启 `gamelogic`；若用 etcd 会重新 Register；否则 Gateway 静态 `logic_addrs` 不变 |
| 注意 | 多 Logic 时仅该 shard 玩家受影响 |

## World

| 项 | 说明 |
|----|------|
| 操作 | `kill` world |
| 预期 | 邮件/聊天骨架失败；场景 Logic 仍可用 |
| 恢复 | 重启 `world`；确认 `world_addrs` / etcd |

## Session

| 项 | 说明 |
|----|------|
| 操作 | `kill` session |
| 预期 | 新 Login/Reconnect/ValidateToken/Bind 失败；已转发的无状态请求仍可能成功 |
| 恢复 | 重启 `session`（Redis 数据保留）；Gateway 重连 Channel（重启 Gateway 或依赖进程内重试策略） |
| 回退 | 清空 `session_addrs`，Gateway/Logic 走本地 `SessionStore`（需本机 Redis） |

## GameDB

| 项 | 说明 |
|----|------|
| 操作 | `kill` gamedb |
| 预期 | `ClaimMailAttachments` RPC 失败；World 其它骨架可能仍响应 |
| 恢复 | 重启 `gamedb`；outbox 未发布行保留，Publisher 继续 |
| 回退 | 清空 `gamedb_addrs`，World 使用进程内 `AsyncMysqlGameDbRepository` |

## 验收清单

- [ ] 杀 Gateway → 重启后 Login 成功
- [ ] 杀 Session → 重启后 Login 成功；杀服期间 Login 明确失败
- [ ] 杀 GameDB → 领取邮件失败；重启后可领取；无 NATS 时 outbox 仅日志+MarkPublished
- [ ] 杀 World / Logic → 对应域失败、其它域可用
