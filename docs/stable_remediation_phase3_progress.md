# 稳定整改三阶段 — 阶段三进度

对照：`docs/GameMesh_Cursor_Server_Stable_Remediation_3_Phases.md`
基线含阶段一/二工作区改动。**阶段三门禁未全部达标 → `STABLE BLOCKED`。**

| 项 | 内容 | 状态 |
|----|------|------|
| 可重复构建 | `install_deps.sh` / `check_deps.sh`（含 sanitizer/shellcheck 提示）；Release 默认 `build-release/` | **已做** |
| 硬门禁脚本 | `stable_gate.sh --full`（短时长强制 BLOCKED） | **已做** |
| Sanitizer | ASan / UBSan / TSan（分目录；TSan 需 `libtsan`；brpc 内部竞态 suppress） | **冒烟 PASS** |
| 发布/Runbook | `docs/release/server-stable-v0.1.0.md` + `docs/runbook/*` | **已写** |
| 可观测 | OpsMetrics：mailbox / placement recover / gamedb unknown_result | **已做** |
| E2E 20× | 默认 20 轮；本机仅冒烟 **2** 轮 | **未达正式门槛** |
| 30min 负载 | 本机仅 **60s** smoke | **未达正式门槛** |
| 2h soak | 本机仅 **90s** smoke | **未达正式门槛** |

### 本阶段额外修复

- `game/GatewayLoginRoute.h`：`RememberBind` 在 `ENABLE_BRPC=OFF` 可编译（ASan/UBSan）
- `CMakeLists.txt`：`EXECUTABLE_OUTPUT_PATH` → `${CMAKE_BINARY_DIR}/test/`；`find_package(Threads)` 提前（TSan 配置）
- `PlayerSerialQueue::Stop`：先 `started_=false` 再 join/析构，避免异步 Complete 与 cv 析构竞态；单测改为可 join 辅助线程
- `test_sanitizers.sh`：TSan + BRPC 时 `MYSQL=ON`；`tools/tsan_suppressions.txt` 抑制未 TSan 重编的 brpc/bthread
- `test_e2e_20x.sh`：`START_CLUSTER=1` 时先 stop 再 start（避免已运行集群 exit 1）
- `scripts/build.sh`：Release 默认写入 `build-release/`，避免覆盖 Debug E2E 二进制
- 本机 3.5Gi 内存：`GAMEMESH_JOBS=2`，否则并行链接易被 OOM kill

### 门禁证据（本机实测）

| 命令 | 结果 | 日志 |
|------|------|------|
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug` | PASS | `/tmp/p3_dbg3.log` |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Release` | PASS | `/tmp/p3_rel2.log`（当时仍写 `build/`；之后脚本已改独立目录） |
| `./scripts/test.sh unit` | PASS | `/tmp/p3_unit.log` |
| `./scripts/test.sh integration` | PASS | `/tmp/p3_int2.log` |
| `./scripts/test_sanitizers.sh asan` | PASS | `/tmp/p3_asan3.log` |
| `./scripts/test_sanitizers.sh ubsan` | PASS | `/tmp/p3_ubsan3.log` |
| TSan `channel_snapshot_race` + `player_serial_async`（suppressions） | PASS | 交互复跑；`/tmp/p3_tsan6.log` 为修复前失败证据 |
| `START_CLUSTER=1 E2E_ROUNDS=2 ./scripts/test_e2e_20x.sh` | PASS | `/tmp/p3_e2e3.log` |
| `LOAD_DURATION_SEC=60 LOAD_CONCURRENCY=8 ./scripts/load_tcp_baseline.sh` | PASS 100% | `/tmp/p3_load61.log` |
| `SOAK_DURATION_SEC=90 ./scripts/soak_test.sh` | PASS | `/tmp/p3_soak91.log` / `run/soak/soak_20260810T093749Z.txt` |
| `./scripts/stable_gate.sh --full`（正式时长） | **未执行** | — |

### STABLE BLOCKED 阻塞项

1. **E2E 未满 20 轮**
   复现正式：`START_CLUSTER=1 E2E_ROUNDS=20 ./scripts/test_e2e_20x.sh`
2. **负载未满 30 分钟**
   `LOAD_DURATION_SEC=1800 LOAD_CONCURRENCY=32 ./scripts/load_tcp_baseline.sh`
3. **soak 未满 2 小时**（或同 commit 的 `SOAK_REPORT`）
   `SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh`
4. **完整 `./scripts/stable_gate.sh --full`** 未在本机一次跑通（含 Phase2 failover 全集）
5. **未创建 tag / 未人工审核**（文档要求）

冒烟缩短跑法（**不算**正式稳定）：

```bash
E2E_ROUNDS=2 LOAD_DURATION_SEC=60 SOAK_DURATION_SEC=90 ./scripts/stable_gate.sh --full
# 预期结尾: STABLE BLOCKED（时长不足）
```

### 遗留风险

- 低内存机并行链接 OOM；建议 `GAMEMESH_JOBS=2`
- TSan 对系统 brpc 依赖 suppressions；彻底方案需用 TSan 重编 brpc
- Redis Registry 仍是控制面单点
- Mail BatchClaim 仍可能同步阻塞 shard（阶段二遗留）
- `placement_recovery_test` 偶发 timing flake（重跑 PASS）

### 判定

```text
STABLE BLOCKED
```

不创建 commit/tag/push。完成上述正式时长门禁并人工审核后，才可候选 `server-stable-v0.1.0-rc1`。
