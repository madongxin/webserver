# 运维基础 Runbook（阶段 3）

## 启动 / 停止

```bash
./scripts/bootstrap.sh
./scripts/build.sh Debug
./scripts/start_cluster.sh          # GAMEMESH_FORMAL=1
./scripts/smoke.sh
./scripts/stop_cluster.sh           # SIGTERM → 等待 → SIGKILL
```

## 健康检查

- Liveness: `GET /api/liveness`
- Readiness: `GET /api/readiness`（draining 时非 200）
- Version: `GET /api/version`

正式模式 HTTP 默认 `127.0.0.1`；探活请本机 curl。

## 故障要点

| 组件 | 现象 | 动作 |
|---|---|---|
| Redis | Session/Placement 失败 | 检查 redis.cnf；集成测试应失败而非 SKIP |
| GameDB | Auth Register/Login 失败（FORMAL） | 查 gamedb_addrs / MySQL |
| GameLogic kill | Map RECOVERING | `kill_logic_and_recover.sh` + MigrateMap |
| 管理口误暴露 | 公网可访问 /gm | 确认 FORMAL + HTTP_BIND=127.0.0.1，ADMIN=0 |

## 已知单点

- 单 Redis / 单 MySQL（开发拓扑）
- Session Gateway 客户端仍取 `session_addrs` 首地址
- Placement 无持久化快照（见 PHASE2_STATUS）
