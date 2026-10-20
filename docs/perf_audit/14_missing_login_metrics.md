# 14 登录侧建议埋点（轻量）

| 位置 | 事件字段 | 用途 |
| --- | --- | --- |
| `GatewayLoginOrchestrator` 各 RPC 前后 | `stage`, `player_id`, `conn_id`, `duration_ms`, `error` | 阶段 P95 |
| `LoginAdmission` 拒绝 | `stage`, `reason`, `inflight` | 区分保护 vs 饱和 |
| `SessionStore::AcquireSession` | `lua_us`, `redis_wait_us` | Redis 是否先抬升 |
| `GameLogicServiceImpl::BindPlayer` | `load_player_ms`, `db_pool_wait_ms` | GameDB/MySQL |
| `GatewayEnterMapOrchestrator` | `resolve_ms`, `transfer_ms`, `reply_bytes` | 进图尾延迟 |
| 压测前后 `/metrics` 快照脚本 | 全量 counter | 窗口差分 |

禁止：为吞吐关闭鉴权或关掉 admission。
