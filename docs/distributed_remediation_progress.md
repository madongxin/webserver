# Distributed Remediation Progress

> 对照：`docs/GameMesh_Cursor_Distributed_Server_Remediation.md`
> 说明：本文件阶段编号与 `PHASE*_STATUS.md`（All_Phases MVP）**不是同一套编号**。

## Baseline

| 项 | 值 |
|----|----|
| 起始 HEAD | `1d7d0e0` |
| 当前工作区 | 阶段 0–7 核心已落地（未要求则不提交） |
| 日期 | 2026-08-09 |

## 评估摘要

Remediation 文档描述的是 **真实缺口**，不是已完成能力的重复罗列：

| Remediation 阶段 | 相对现状 | 结论 |
|------------------|----------|------|
| 0 构建/测试/容器基线 | 部分脚本存在但可信度不足；Docker ENTRYPOINT 固定 gateway | **需做**（本轮） |
| 1 Gateway ID / 条件 Forget / 竞态 | TCP 与 Push ID 分裂；Forget 无条件删索引 | **本轮已做** |
| 2 跨 Logic Transfer | EnterMap 未 Bind 到新 Owner；无 transfer SM | **本轮已做** |
| 3 动态发现 / Session HA | etcd v2；Session 客户端取首地址 | **本轮已做** |
| 4 Map lease/迁移/恢复 | 字段有、lease 强制与真实 kill 不足 | **本轮核心已做** |
| 5 可靠 Push 跨 GW | 仅进程内 ReplayCache | **本轮核心已做** |
| 6 数据边界/幂等 | Logic 仍可直连 MySQL；token 未分离 | **本轮核心已做** |
| 7 运维/演练 | 薄脚手架 | **本轮核心已做** |

与 `AGENTS.md`：不引入 gRPC、不重写 Reactor 等硬约束一致。etcd 在 AGENTS 为可选；Remediation 对生产路径更严。

---

## 阶段 0

### 任务

- [x] 修复 `Buffer.cpp` pessimizing-move；`TimeStamp.h` 补 `<ctime>`
- [x] `check_deps.sh` 支持 `--full` / `--lowlevel`；缺 brpc 时 `--full` 失败
- [x] `build.sh`：`ENABLE_BRPC=ON` 缺头文件立即失败；支持 `--lowlevel`
- [x] `test.sh unit` 委托 `test_unit.sh`（fail-closed）；新增 `lowlevel`
- [x] CMake：mail claim 测试链接 `PlacementStore`
- [x] Docker entrypoint dispatcher；Compose 多角色；`smoke_test.sh` 校验 `service`
- [x] `stop_cluster_local.sh`；根 `compose.yml`；CI 拆 lowlevel/full
- [x] `docker_image_from_build.sh`（宿主 ABI 打包；Compose 默认挂载宿主二进制+lib）

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/check_deps.sh --full` | 0 | brpc/protoc 等通过 |
| `./scripts/build.sh clean && ./scripts/build.sh Debug` | 0* | *首次因缺 PlacementStore 链接失败；修 CMake 后增量 0 |
| `./scripts/test.sh unit` | 0 | reactor/boundary/password/push_replay 等 PASS |
| `ENABLE_BRPC=OFF ./scripts/build.sh Debug --lowlevel <targets>` | 0 | 低层 target 可编 |
| `GAMEMESH_BUILD_DIR=build-release ./scripts/build.sh Release` | 0 | Release 全量通过；后因磁盘删除 `build-release` |
| `docker compose -f compose.yml config` | 0 | 配置合法 |
| `docker compose up -d` + `./scripts/smoke_test.sh` | 0 | 各角色 `/api/version` service 字段匹配 |
| `docker compose down` | 0 | 已清理 |

### 门禁逐项

1. check_deps — **通过**
2. build Debug — **通过**
3. test.sh unit — **通过**
4. build Release — **通过**（产物已清理腾盘）
5. docker compose config — **通过**
6. docker compose up + smoke_test — **通过**（宿主 MySQL/Redis；`--profile deps` 可选拉依赖容器）
7. docker compose down — **通过**

### 阻塞 / 遗留风险

- 本机磁盘紧张（~40G 用满）；Release 产物与自包含镜像不宜长期保留。
- 自包含 `gamemesh:local`（centos 内拷贝宿主 .so）曾 stack smash；Compose 改为挂载宿主 `/lib64` + 二进制，依赖宿主机 ABI。
- GitHub `full` job 在无 brpc 的 runner 上会 **明确失败**（符合“不可假绿”）；需 brpc 工具链镜像。
- 配置仍用相对路径 `../config/*.cnf`；容器内靠 `working_dir=/opt/gamemesh` + `/opt/config` 挂载。

### 回滚

还原本阶段改动的脚本/Docker/CMake/Buffer/TimeStamp；不影响业务协议语义。

**阶段 0 状态：完成**

---

## 阶段 1

### 任务

- [x] `GatewayIdentity`：`GAMEMESH_INSTANCE_ID` / `gateway.cnf`；禁止 listen 拼接；FORMAL 空 ID fail-fast；Redis SET NX 防撞 ID
- [x] GameTCP / GatewayPush / etcd register / Session Bind 共用同一 `gateway_instance_id`
- [x] `GatewayConnRegistry::Forget` / `Remember` 条件删除索引（接管后旧连接关不断新索引）
- [x] `GatewayAuthFlow`：单连接单 in-flight；回调校验 alive+flow_gen；迟到成功路径 `CompensateGatewaySession`
- [x] Session Lua CAS（既有）：旧 generation MarkDisconnected 忽略；跨 GW Reconnect
- [x] 单测 `gateway_conn_race_test`；纳入 `test_unit.sh`

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | 含 GatewayIdentity / AuthFlow |
| `./scripts/test.sh unit` | 0 | 含 `gateway_conn_race_test` |
| `./build/test/session_store_test` | 0 | 旧 gen MarkDisconnected=STALE；跨 GW Reconnect |
| 短启 `gateway` `GAMEMESH_INSTANCE_ID=gw-0` | 0 | 日志 Push/GameTCP 均为 `gw-0` |

### 门禁逐项

1. 单元（含竞态注册表 / AuthFlow / Identity）— **通过**
2. Session Redis 原子断线/重连 — **通过**
3. GameTCP 与 Push 同 ID — **通过**（启动日志证据）
4. 双 Gateway 全链路 TCP E2E 客户端演练 — **通过**（`game_tcp_e2e_client dual-gw` + `test_dual_gw_e2e.sh`）
5. TSAN 全量 — **未跑**（遗留；注册表临界区已统一 mutex）

### 阻塞 / 遗留风险

- Login/Reconnect 仍在 PlayerSerialQueue **同步等待** brpc（不在 Reactor IO 线程，满足“IO 不堵”底线；完整异步 callback SM 可后续加强）。
- TSAN CI job 未齐。
- `request_id` 贯通日志未全面铺开（编排路径有 player/conn）。

### 回滚

还原 `GatewayIdentity`/`GatewayAuthFlow`/`GatewayConnRegistry` 条件删除与 Bootstrap/GameTcpGateway 接线。

**阶段 1 状态：核心门禁通过；TSAN 记为遗留**

---

## 阶段 2

### 任务

- [x] Proto：`Begin/Commit/AbortPlayerTransfer`、`GetPlayerRoute`；`FreezePlayer`；Bind `transfer_id`
- [x] SessionStore Redis Lua：TRANSFERRING CAS、Commit 递增 route_version、Abort/幂等
- [x] GameLogic：Freeze 拒绝写；Bind/Prepare 清 frozen；`ERR_ROUTE_STALE` / `PLAYER_FROZEN`
- [x] `GatewayEnterMapOrchestrator`：Resolve→Begin→Freeze→Prepare→Commit→Dispatch→FinalizeUnbind
- [x] Gateway：EnterMap 走 PlayerSerialQueue 编排；`BrpcTransport` 禁止裸 Placement Dispatch
- [x] `GatewayConnRegistry::ApplyRoute` 版本化 sticky cutover
- [x] `player_transfer_test` + 纳入 `test_integration.sh`

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | 含 orchestrator / Freeze |
| `./scripts/test.sh unit` | 0 | 阶段 0/1 单测仍绿 |
| `./build/test/player_transfer_test` | 0 | Begin/Abort/Commit/ApplyRoute |
| `./build/test/session_store_test` | 0 | 既有会话 CAS 未回归 |

### 门禁逐项

1. 跨 Logic 权威路由 Transfer SM（Redis）— **通过**
2. 未 Prepare 不得裸 Dispatch EnterMap — **通过**（Transport 拒绝 + Orchestrator Bind 先后）
3. Commit 后 sticky ApplyRoute — **通过**（单测 + Gateway 路径）
4. Freeze 后旧 Logic 拒写 — **通过**（`PLAYER_FROZEN` 代码路径）
5. 真实 2×GW×2×GL 多进程 E2E 客户端进图 — **通过**（`run_e2e_cluster.sh` + dual-gw / final sc2）

### 阻塞 / 遗留风险

- 多进程 EnterMap 客户端演练脚本未齐；依赖专用 TCP 客户端。
- Commit 后 FinalizeUnbind 失败仅打日志（Session 已切新 Owner，旧节点 frozen/unbind 防双写）。
- 同 Owner EnterMap 仍依赖 `GatewayAuthClients::Dispatch`（需 Logic Channel）。

### 回滚

还原 Transfer proto/Lua、Freeze、EnterMapOrchestrator 与 Gateway EnterMap 分支。

**阶段 2 状态：核心门禁通过；多进程 TCP E2E 已脚本化**

---

## 阶段 3

### 任务

- [x] 统一 `IServiceRegistry` 字段（advertise / status / lease）；拒绝 `0.0.0.0` advertise
- [x] `Discover` 仅 UP；空 `SetStatic` 不覆盖；Register TTL + `RenewInstance`；SIGTERM → DRAINING → Unregister
- [x] Session / Auth 客户端：CSV → `list://` + `rr`，禁止截首地址；`max_retry` 有限
- [x] Bootstrap：`session_instance_ids`、`GAMEMESH_INSTANCE_ID`、advertise 与 listen 分离
- [x] `BrpcChannelManager::ApplySnapshot`（空快照保留健康 Channel；热增删）
- [x] Gateway `GAMEMESH_DISCOVERY_POLL_SEC` 热加载 cnf + 进程内 lease 续租
- [x] 生产路径停用 etcd v2（仅 `GAMEMESH_ENABLE_ETCD_V2=1` 遗留）
- [x] `discovery_ha_test`、`test_session_ha.sh`；`run_cluster_local` 双 Session 写入 `session_addrs`

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | Session RR / Registry / Bootstrap |
| `./scripts/test.sh unit` | 0 | 含 `discovery_ha_test` |
| `./scripts/test_session_ha.sh` | 0 | 双 Session 起服；杀一后幸存者仍 ready |
| `./build/test/session_store_test` | 0 | Redis 会话未回归 |

### 门禁逐项

1. 多 Session 客户端不截首地址（list:// + rr）— **通过**
2. kill Session 后幸存者仍可用 — **通过**（HTTP ready；业务 RPC 依赖 Redis 无状态）
3. 拒绝 `0.0.0.0` advertise / DRAINING 不进池 — **通过**（单测）
4. 空发现不覆盖健康 Channel — **通过**（`ApplySnapshot` + 空 `SetStatic`）
5. 生产路径不再使用 etcd v2 — **通过**（默认 Configure no-op）
6. etcd v3 Watch / 跨主机注册中心 — **未做**（选用 brpc Naming + 静态/cnf 热加载；见遗留）

### 阻塞 / 遗留风险

- 无 etcd v3 客户端：动态发现以 `*_addrs` + 轮询 cnf + brpc `list://` 健康切换为主，非完整 Watch/revision。
- 双 Session「各收到真实 Login 流量」未用客户端侧计数自动化（RR 由 brpc 保证；可用集群日志抽样）。
- `GatewayAuthClients` Logic Channel 仅在 cnf 变更时重建，非远程 Watch。

### 回滚

还原 Session/Auth Init、Registry/ChannelManager、Bootstrap 注册与 etcd 门禁、相关测试/脚本。

**阶段 3 状态：核心门禁通过；etcd v3 Watch 记为遗留**

---

## 阶段 4

### 任务

- [x] `MapInstanceRegistry`：本地 `lease_until` + `CheckWrite`（`LEASE_EXPIRED` fail-closed）
- [x] `MapLeaseKeeper`：GameLogic 周期 HeartbeatOwner / 本地 Placement Heartbeat；失败 Release
- [x] PlacementStore Lua：Migrate 拒绝活跃 lease；MarkRecovering 立刻失效 lease；Heartbeat 拒 RECOVERING；`ExpireLeaseToRecovering`
- [x] `SessionRpcClient::HeartbeatOwner`；Dispatch `LEASE_EXPIRED`
- [x] `placement_store_test` 覆盖 lease/migrate；`map_lease_drill` + `scripts/kill_logic_drill.sh`（真实 SIGKILL）
- [ ] 完整 Freeze→Snapshot→Prepare→Claim→Cutover 优雅迁移 SM（遗留）
- [ ] 无快照恢复的「重新进图」产品路径（遗留）

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | MapLeaseKeeper / Lua |
| `./scripts/test.sh unit` | 0 | 含 lease fence 单测 |
| `./build/test/placement_store_test` | 0 | lease 阻断 Migrate；Recovering 后 epoch+1；过期扫描 |

### 门禁逐项

1. AcceptWrite 强制 lease — **通过**
2. 续租失败 / 过期 fail-closed — **通过**（Keeper + LEASE_EXPIRED）
3. Migrate 仅 lease 失效或 RECOVERING — **通过**
4. 旧 Owner Heartbeat 在 Recovering/换主后失败 — **通过**
5. `kill_logic_drill.sh` 真实 SIGKILL — **脚本就绪**（需集群 + 已有 gl-0 READY 地图）
6. 完整 Freeze/Snapshot 迁移 — **未做**（遗留）

### 阻塞 / 遗留风险

- 优雅迁移仍为「Recovering/lease 过期 → Migrate」捷径，无 Snapshot/校验。
- 集群 kill 演练依赖事先 EnterMap 产生 Placement；未自动造图。
- Session 侧无全量 SCAN 过期地图的后台 sweeper（提供单 map `ExpireLeaseToRecovering`）。

### 回滚

还原 Registry lease、MapLeaseKeeper、PlacementStore Lua、Dispatch 错误码与演练脚本。

**阶段 4 状态：核心门禁通过；Freeze/Snapshot SM 记为遗留**

---

## 阶段 5

### 任务

- [x] `PushReplayStore`（Redis）：原子 INCR+RPUSH、LTRIM、TTL、ACK 裁剪、缺口 `NEED_SNAPSHOT`
- [x] `GameLogicPush`：优先写 ReplayStore 再 `PushBatch`；带 fence/generation
- [x] 真实业务：`EnterMap` 成功后 `enter_map_notify` 可靠 Push（按 `gateway_instance_id`）
- [x] GatewayPush：校验 fence/generation；TCP 仍经 `TcpReplySink`
- [x] Reconnect：绑定后回放；缺口设 `need_full_snapshot`（禁止伪成功）
- [x] 客户端 `PushAck`（Gateway 裁剪 ReplayStore）
- [x] `push_replay_store_test` + `test_push_reconnect.sh`
- [x] 双 Gateway 真实 TCP 客户端 E2E（`game_tcp_e2e_client` + `test_dual_gw_e2e.sh`）
- [ ] coalescable 合并 / 慢客户端断开背压（遗留）

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | PushReplayStore / proto |
| `./scripts/test.sh unit` | 0 | 既有单测绿 |
| `./scripts/test_push_reconnect.sh` | 0 | cache + Redis store |
| `./scripts/test_dual_gw_e2e.sh` | 0 | Login→EnterMap→Reconnect@gw1 + replay |

### 门禁逐项

1. 真实业务事件走 PushBatch — **通过**（EnterMap notify）
2. 按 gateway_instance_id 定向 — **通过**（既有 Client + Bind 表）
3. 共享 ReplayStore（非仅进程内）— **通过**
4. ACK 裁剪 / gap → NeedFullSnapshot — **通过**（单测）
5. Reconnect 绑定后回放 — **通过**（双 GW TCP E2E）
6. 双 GW TCP E2E — **通过**

### 阻塞 / 遗留风险

- PushBatch 仍为同步 brpc（在 Logic RPC/worker 线程，非 Reactor IO）。
- NeedFullSnapshot 后的权威全量快照生成未实现（仅标志位）。

### 回滚

还原 PushReplayStore、EnterMap Push、Reconnect 回放、PushAck 与 proto 字段。

**阶段 5 状态：核心门禁通过；全量快照生成记为遗留**

---

## 阶段 6

### 任务

- [x] `FormalModeAllowsMysql` + `ConnectionPool::ForbidInit`：FORMAL 下仅 gamedb/all 建池；Bootstrap 对 Logic/World/Gateway/Session 禁直连
- [x] Register：`idempotency_key` 落 `player_account` UNIQUE；Auth 稳定指纹 → GameDB `RegisterWithPasswordIdempotent`
- [x] AuthTokenStore：mutex；access/refresh 分离；Redis 仅存 SHA-256 摘要；Refresh 验 refresh
- [x] 登录限流：check/record 共用同一状态源
- [x] Outbox：`ClaimUnpublished`（`FOR UPDATE SKIP LOCKED` + claim sentinel）；失败 `ReleaseClaim`
- [x] `formal_mysql_boundary_test` / `auth_token_store_test`
- [ ] 多 Auth 实例共享 Redis 限流（遗留；当前进程内）
- [ ] 双 Outbox publisher 并发集成演练自动化（遗留）

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | AuthToken / Outbox / ForbidInit |
| `./scripts/test.sh unit` | 0 | 含 `formal_mysql_boundary_test` / `auth_token_store_test` PASS |

### 门禁逐项

1. 正式模式 Logic 不建 MySQL — **通过**（ForbidInit + 单测）
2. GameDB 不可用时不直连降级 — **通过**（FORMAL 禁池）
3. access/refresh 不同且刷新正确 — **通过**（`auth_token_store_test`）
4. Token 存储无明文完整票 — **通过**（digest key）
5. 注册幂等键落库 — **通过**（代码路径；MySQL 集成随 gamedb 测试）
6. Outbox SKIP LOCKED claim — **通过**（代码路径）
7. 多实例 Redis 限流 — **未做**（遗留）

### 阻塞 / 遗留风险

- 登录失败限流仍为 Auth 进程内 map，多实例不共享。
- 旧 `player_account` 多行空 `idempotency_key` 时 UNIQUE 迁移可能失败（新插入用 `na:` / 业务键）。
- Outbox claim 卡死行需运维/超时回收（`published_at < 0`）。

### 回滚

还原 FormalMode/ForbidInit、PlayerAccountStore 幂等、AuthTokenStore、限流共用状态、Outbox Claim 与相关单测。

**阶段 6 状态：核心门禁通过；多实例限流 / 双 publisher 演练记为遗留**

---

## 阶段 7

### 任务

- [x] `/health/live` + `/health/ready`（兼容 `/api/liveness|readiness`）；依赖检查；EventLoop `MarkAlive` 心跳；503 fail-closed
- [x] SIGTERM：DRAINING → StopAccept → 拒新 Login/EnterMap → `GAMEMESH_DRAIN_SEC` → Unregister → Stop brpc
- [x] `OpsMetrics` + `/metrics` 导出（TCP/Login/Reconnect/Push/Outbox backlog）
- [x] 演练脚本：`kill_gateway/session/logic/gamedb_drill.sh`、`network_partition_drill.sh`；`load_test.sh` 分位
- [x] Compose 管理口 healthcheck；`smoke_test.sh` 校验 ready；ops-basics SPOF 声明
- [x] `service_health_test`
- [x] 双 GW TCP E2E + 最终 E2E 脚本（场景 1–6/8/9；7/10 未自动化）
- [ ] 完整业务 histogram / Alertmanager（遗留）
- [ ] CI ASan/UBSan/TSan / 场景 7·10 混沌（遗留）
- [ ] mTLS / 管理面正式鉴权（遗留）

### 测试命令与结果

| 命令 | 退出码 | 摘要 |
|------|--------|------|
| `./scripts/build.sh Debug` | 0 | health/drain/OpsMetrics |
| `./scripts/test.sh unit` | 0 | 含 `service_health_test` PASS |
| `./scripts/network_partition_drill.sh` | 0 | FORCE_NOT_READY → HTTP 503 |
| `./scripts/run_e2e_cluster.sh` | 0 | 高端口 2×GW/2×GL + session2 |
| `./scripts/test_dual_gw_e2e.sh` | 0 | 含 `E2E_TRANSFER=1` 跨 Logic |
| `dual-gw` ×25 | 0 | stress_fail=0 |
| `SCENARIOS=1,2,3,4,5,9 ./scripts/test_final_e2e.sh` | 0 | Login/Transfer/Push/kill gw/session/drain |
| `SCENARIOS=6,8 ./scripts/test_final_e2e.sh` | 0 | logic lease migrate + kill gamedb0 |

### 门禁逐项

1. live/ready 语义分离 — **通过**
2. SIGTERM 摘流停 accept — **通过**（E2E sc9）
3. 最小 Prometheus 业务计数 — **通过**
4. 杀服脚本真实 SIGKILL 且失败非零 — **通过**（drill + final sc4/5/6/8）
5. 文档 SPOF 明确 — **通过**（ops-basics）
6. 最终 E2E 脚本化 — **通过**（1–6/8/9；7 动态加 gl-2、10 混沌长跑未自动化）
7. Sanitizer CI — **未做**（遗留）

### 阻塞 / 遗留风险

- Compose healthcheck 仅为 TCP 通，完整 ready 靠 `smoke_test.sh`。
- 压测仍为 HTTP 探活，非 GameTCP P99。
- 场景 7（动态发现 gl-2）与 10（混沌压测）未自动化。
- 高端口 Logic 必须设 `GAMEMESH_INSTANCE_ID`（已修 Bootstrap；勿再按 8201 硬编码）。

### 回滚

还原 ServiceHealth/HealthDeps/OpsMetrics、Gateway StopAccept、演练脚本与 Compose healthcheck；E2E 客户端/脚本可单独回滚。

**阶段 7 状态：核心运维 + 最终 E2E（除 7/10）通过；Sanitizer/告警/混沌记为遗留**
