# Urgent Minimum Stable Fix — 阶段二进度与验收

基线：`857d963`
文档：`docs/GameMesh_Cursor_Urgent_Minimum_Stable_Fix_857d963.md`
状态：**阶段二代码 + 快速门禁 + 长门禁均已执行；结论 STABLE BLOCKED**

按文档：不创建 Tag、不 Push；未降 load/soak 阈值。

## 修改范围（相对阶段一增量）

### CI / 依赖

- `.github/workflows/ci.yml` / `nightly-stable.yml`：MySQL 8 service；`git show --check` 去掉 `|| true`；sanitizer 包按 apt-cache 发现；brpc 安装硬化；Redis/MySQL readiness
- `scripts/test_integration.sh`：测前等待 Redis/MySQL
- `scripts/install_deps.sh`：与 CI 对齐的依赖安装

### Failover 真绿

- `scripts/test_session_failover.sh`：Redis ONLINE 硬断言（连接 hold 窗口）；旧 fence 真实 Dispatch → `FENCE_REJECT`；禁止过期 ticket 重试
- `scripts/test_gamedb_unknown_result_failover.sh`：RPC fail / SUCCEEDED / bag+ver / outbox=1 / survivor 写
- `test/logic_dispatch_tool.cpp`、`test/gamedb_rpc_tool.cpp`（inventory）、CMake 目标
- `client/game_tcp_e2e_client.cpp`：`PrintKv`/`error=` 后 `fflush`（避免管道全缓冲导致脚本读空）

### Soak / stable_gate

- `scripts/soak_test.sh`：父 shell 更新 `rss0/fd0/thr0/proc_exits`；末次 `sample_once` 失败不再吞掉
- `scripts/stable_gate.sh`：`git show --check HEAD`；dirty → BLOCKED；export 失败不吞；实验测默认跳过

### Experimental 收紧

- `runtime/FormalMode.h`：`ExperimentalFeatureEnabled`
- `apps/ServerBootstrap.cpp`：PlacementRecovery 仅 `GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY=1`
- `scripts/run_e2e_cluster.sh` / `test_final_e2e.sh` / `e2e_inventory.sh`：默认关动态扩容；scenario 7 跳过；health 忽略 gl-2（实验关时）
- `docs/release/server-stable-v0.1.0.md`：实验能力说明

## 快速门禁结果（本机，工作树 dirty）

| 命令 | 结果 |
|------|------|
| `./scripts/check_deps.sh --full` | PASS |
| `./scripts/build.sh Debug` | PASS |
| `./scripts/build.sh Release` | PASS |
| `./scripts/test.sh unit` | PASS |
| `./scripts/test.sh integration` | PASS |
| `./scripts/final_e2e.sh --start-cluster` | PASS |
| `./scripts/test_session_failover.sh` | PASS |
| `./scripts/test_gamedb_unknown_result_failover.sh` | PASS |
| `./scripts/test_sanitizers.sh all` | PASS |

## 长门禁结果（真实时长，未降阈值）

日志目录：`run/long_gate/`（`summary.txt` / `results.env` / 分步 `*.log`）

| 项 | 结果 | 证据 |
|------|------|------|
| `E2E_ROUNDS=20` | **PASS**（136s） | `run/long_gate/e2e_20x.log` |
| `LOAD 1800s / conc=32` | **FAIL** | 成功率 **52%** &lt; 95%；ok=3502 fail=3185；dual p50≈8.0s（接近超时）；`run/load/load_20260811T041158Z.{txt,json}` |
| `SOAK 7200s` | **FAIL** | 成功率 **88%** &lt; 90%；ok=2232 fail=303；无进程异常退出；RSS 354→423MB；`run/soak/soak_20260811T044216Z.{txt,json,samples}` |
| `stable_gate --full` | **BLOCKED** | dirty=39；`git show --check HEAD` 命中基线提交文档尾随空白；`run/stable_gate/summary_1786430537.json` |
| 干净 working tree | **否** | 阶段一+二未提交 |
| GitHub lowlevel/full | **未验证** | 未 Push |

## 结论

```text
STABLE BLOCKED
```

不可创建 `server-stable-v0.1.0-rc1` Tag。

阻塞项（按优先级）：

1. **Load 32 并发成功率 52%**（阈值 95%，未自行降低）— dual-gw 延迟顶到 ~8–9s 超时带
2. **Soak 成功率 88%**（阈值 90%）
3. **工作树 dirty** + HEAD 文档尾随空白导致 `stable_gate` 无法 PASS
4. **GitHub CI** 尚未在推送后验证

Release manifest（仍 BLOCKED 时导出）：`run/release/857d9637dfe5fdc523e424433f4b9694efd63ce6/manifest.json`
