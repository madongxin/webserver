# 阶段 2 状态（GameMesh_Cursor_All_Phases）

> 更新：2026-08-08
> 对照：`docs/GameMesh_Cursor_All_Phases.md` 阶段 2

## 已落地

### 2.1 权威 Map Placement（Session 模块）
- `PlacementStore`：Redis Lua 原子创建 / Get / Migrate / MarkRecovering / Heartbeat
- 全局 `map_instance_id`：`INCR {prefix}map:idgen`
- Template 索引 `{prefix}map:tpl:{realm}:{template}` → 并发创建同一目标地图唯一实例
- 状态字段：CREATING/READY/FROZEN/MIGRATING/RECOVERING/CLOSED（迁移路径 READY↔RECOVERING）
- Session RPC：`ResolveOrCreateMap` / `GetPlacement` / `MigrateMap` / `MarkRecovering` / `HeartbeatOwner` / `UpdatePlayerRoute`
- 进程内 `MapPlacement` 降级为缓存（`UpsertCache`），非事实源

### 2.2 进图流程
- Gateway `BrpcTransport`：EnterMap/Leave/Ping 走 Session Placement（或本地 PlacementStore），刷新 sticky 路由
- GameLogic `HandleEnterMap`：Claim → `UpdatePlayerRoute`（fence CAS）；失败回滚本地玩家归属
- RECOVERING/CLOSED/FROZEN 拒绝作为写路由

### 2.3 迁移 / 恢复（MVP）
- Migrate：epoch+1、owner 切换、幂等 `idempotency_key`
- MarkRecovering + `kill_logic_and_recover.sh` 辅助
- **限制**：无持久化快照/日志重放；内存 Claim 表进程内有效，**非生产级无损迁移**

### 2.4 服务发现（路径选择）
- **决定**：短期不扩展 etcd v2；正式路径规划 **etcd v3 lease/watch**（或 brpc NamingService）
- 现状：`StaticServiceRegistry` + 配置 `*_addrs`；注册用 `GAMEMESH_ADVERTISE_HOST`（默认 127.0.0.1），禁止把 `0.0.0.0` 当 advertise
- 未知 logic ID 仍 fail-closed

### 2.5 可靠 Push
- 删除 GameLogic 硬编码 `127.0.0.1:8181`；改为 `gateway_push_addrs` / Registry 发现
- Push 校验 `gateway_instance_id` 与本机一致 + session 绑定
- `PushReplayCache`：每玩家有界序列；缺口过大 → need_snapshot
- `PushToBoundGateway` 自动分配 `server_seq` 并写入回放缓存

### 2.6 脚本
- `scripts/test_placement.sh`
- `scripts/test_discovery.sh`
- `scripts/test_push_reconnect.sh`
- `scripts/kill_logic_and_recover.sh`
- `scripts/run_cluster_local.sh`（formal + 第二 session）

## 验证命令

```bash
./scripts/build.sh Debug
./scripts/test_unit.sh
./scripts/test_placement.sh      # 需 Redis
./scripts/test_push_reconnect.sh
./scripts/test_discovery.sh
./scripts/test_reactor.sh
```

## Proto 变更

| 变更 | 兼容 |
|------|------|
| `session.proto` Placement / UpdatePlayerRoute RPCs | 新增；旧客户端忽略 |
| Session Redis `pushEndpoint` 字段 | 可选 |

## 风险 / 未完成

- 双 Session 进程对 Gateway 的 session_addrs 负载均衡尚未做（Init 仍取首地址）
- etcd v3 / NamingService 热更新 Channel 池未实现
- 跨 Gateway Push + 重连回放的完整 E2E 集成测试未自动化
- 地图快照持久化未做（文档已标明限制）
- `force_new` / 多实例负载均衡策略仍为 RR + preferred_owner
