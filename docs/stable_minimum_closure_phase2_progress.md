# 稳定最小闭环 — 阶段二进度

对照：`docs/GameMesh_Cursor_Server_Stable_Minimum_Closure_e8ab08f.md`
基线：`e8ab08f`；**阶段二完成，停止等待确认后再进阶段三。**

| 项 | 状态 |
|----|------|
| E2E `inventory.tsv`（role/instance_id/pid/rpc/http/game）+ 健康检查重建 | **已做** |
| `stable_gate.sh --full`：破坏场景后 stop+restart+ready；机器可读摘要 | **已做** |
| Session 真故障（杀 sess-0 → gw1 登录 + 原玩家 reconnect + 旧 ticket 拒绝） | **已做** |
| Logic 自动恢复（进图→杀 gl-0→scheduler Migrate→gw1 再进图） | **已做** |
| GameDB 未知结果（failpoint abort-after-commit → gamedb-1 query SUCCEEDED → survivor write） | **已做** |
| 动态扩容 gl-2 真进程 + DRAINING 后不再分配 | **已做** |
| Registry 发现键清空保留 Channel；共用 Redis 限制已文档化 | **已做** |
| kill_* 演练按 inventory 查 PID（兼容旧 pids） | **已做** |

### 进程清单 / 健康

- `scripts/e2e_inventory.sh`：`e2e_inv_append` / `e2e_inv_replace` / `e2e_pid_of`（末行优先）/ `e2e_cluster_healthy` / `e2e_ensure_cluster`
- `start_formal.sh` / `run_e2e_cluster.sh` 写 inventory；仅 pids 存活但角色死亡 → 重建

### stable_gate

- 每组破坏脚本后 `restart_e2e_clean`（stop → 清 pids/inventory → start → healthy）
- 摘要：`run/stable_gate/summary_<epoch>.json`（commit、起止时间、步骤 exit_code）

### 故障场景摘要

| 脚本 | 前置 | 注入 | 后置断言 | 日志 |
|------|------|------|----------|------|
| `test_session_failover.sh` | gw0 登录 | SIGKILL sess-0 | gw1 新登录；原玩家 gw1 reconnect；旧 ticket mismatch | `run/e2e/logs/session*.log` |
| `test_logic_auto_recovery.sh` | 进图 gl-0 map | SIGKILL gl-0 + 加速 leaseUntil | owner→gl-1、epoch↑；gw1 再进图 gl-1 | `session.log` PlacementRecovery |
| `test_gamedb_unknown_result_failover.sh` | 集群 | gamedb-0 failpoint abort-after-commit | gamedb-1 `SUCCEEDED`；survivor mutate ver↑ | `gamedb0_fp.log` |
| `test_dynamic_logic_scale.sh` | 集群 | 启 gl-2；DRAINING | 进图 gl-2；SEED_OWNERS 不含 gl-2 时新图≠gl-2 | `logic2.log` |
| `test_registry_outage.sh` | dual-gw 热通道 | 删 `svc:gamelogic:*` + `svcidx` | dual-gw 仍成功；恢复后可写 version 发现 | gw/session logs |

**共用 Redis 限制**：Registry outage 只删发现键，不停 Redis，避免伪装成「仅注册中心」故障时误伤 Session/Placement。

### 门禁证据

| 命令 | 结果 |
|------|------|
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | PASS |
| `./scripts/test.sh unit` | PASS |
| `./scripts/test.sh integration` | PASS |
| `./scripts/final_e2e.sh --start-cluster` | PASS（含 scenario 7 动态扩容） |
| `./scripts/test_session_failover.sh` | PASS |
| `./scripts/test_logic_auto_recovery.sh` | PASS |
| `./scripts/test_gamedb_unknown_result_failover.sh` | PASS |
| `./scripts/test_dynamic_logic_scale.sh` | PASS |
| `./scripts/test_registry_outage.sh` | PASS |

### 主要改动文件

- `scripts/e2e_inventory.sh`、`stable_gate.sh`、`start_formal.sh`、`run_e2e_cluster.sh`
- `scripts/test_{session_failover,logic_auto_recovery,gamedb_unknown_result_failover,dynamic_logic_scale,registry_outage}.sh`
- `scripts/kill_{logic,session,gamedb,gateway}_drill.sh`
- `runtime/brpc/GameDbServiceImpl.cpp`（failpoint）、`test/gamedb_rpc_tool.cpp`
- `test/placement_seed_tool.cpp`（`SEED_OWNERS`）
- `runtime/IGameDbRepository.h`、`MailService.cpp`（同步 Claim 虚接口，修 mail 单测链接）
- `apps/ServerBootstrap.cpp`（`GAMEMESH_PLACEMENT_SCAN_COUNT` / `RECOVER_IV`）

### 遗留（阶段三）

- 可复现 CI / brpc 工具链、正式 20× E2E、30min load、2h soak、`server-stable-v0.1.0` 候选
- 未 commit / 未 push

**请确认后进入阶段三。**
