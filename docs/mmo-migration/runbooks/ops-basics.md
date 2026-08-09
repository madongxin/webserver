# 运维基础 Runbook（阶段 7）

## 启动 / 停止

```bash
./scripts/bootstrap.sh
./scripts/build.sh Debug
./scripts/run_cluster_local.sh   # 或 start_formal.sh / docker compose up
./scripts/smoke_test.sh
./scripts/stop_formal.sh         # SIGTERM → 等待 → SIGKILL（GAMEMESH_RUN_DIR）
```

## 健康检查

| 路径 | 含义 | HTTP |
|------|------|------|
| `GET /health/live`（别名 `/api/liveness`） | EventLoop 心跳存活 | 503=僵死 |
| `GET /health/ready`（别名 `/api/readiness`） | 角色依赖 + 非 draining | 503=不可进 LB |
| `GET /api/version` | service / server_id / build | 200 |
| `GET /metrics` | Prometheus（含 OpsMetrics） | 200 |

正式模式 HTTP 默认 `127.0.0.1`；探活请本机 curl。  
`GAMEMESH_FORCE_NOT_READY=1`：强制 ready 失败（演练）。  
`GAMEMESH_DRAIN_SEC`：SIGTERM 后等待在途秒数（默认 2）。

## 优雅停服（SIGTERM）

```text
SetDraining → ready=false
→ 注册中心 DRAINING
→ Gateway StopAccept + 拒 Login/Register/EnterMap
→ 等待 GAMEMESH_DRAIN_SEC
→ Unregister → Stop brpc → Quit GameTCP
```

## 故障演练脚本（失败非零）

```bash
./scripts/kill_gateway_drill.sh
./scripts/kill_session_drill.sh
./scripts/kill_logic_drill.sh      # 需 READY 地图
./scripts/kill_gamedb_drill.sh
./scripts/network_partition_drill.sh
./scripts/load_test.sh 127.0.0.1 8080 200 /health/live
```

`start_formal.sh` pid 顺序：`session, gamedb0, gamedb1, world, logic0, logic1, gw0, gw1`。

## 故障要点

| 组件 | 现象 | 动作 |
|---|---|---|
| Redis | Session/Placement 失败；session/gateway ready 503 | 查 redis.cnf |
| GameDB | Auth Register/Login 失败（FORMAL） | 查 gamedb_addrs / MySQL |
| GameLogic kill | Map RECOVERING | `kill_logic_drill.sh` |
| 管理口误暴露 | 公网可访问 /gm | FORMAL + HTTP_BIND=127.0.0.1，ADMIN=0 |

## 已知单点（不得宣称存储已 HA）

- **Redis**：单节点（Session / Placement / PushReplay / AuthToken）
- **MySQL**：单节点（GameDB / Outbox）
- **NATS**：可选；未部署则 Outbox 仅落库
- **etcd**：可选；默认 static `*_addrs` + brpc `list://`
- 访问层多实例（Gateway/Session/Logic）≠ 存储 HA

## 灰度 / 回滚

见 `gray-rollback.md`。扩缩容：改 `*_addrs` 或注册表后 Gateway 轮询刷新（`GAMEMESH_DISCOVERY_POLL_SEC`）。
