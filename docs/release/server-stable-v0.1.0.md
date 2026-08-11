# server-stable-v0.1.0（候选）

状态：以 `./scripts/stable_gate.sh --full` 与 `run/release/<commit>/manifest.json` 为准；未满足全部硬门禁时必须输出 `STABLE BLOCKED`，不得宣称 STABLE。

## 本版定位

1. **本版是客户端联调与后续业务开发的稳定服务器基线**（TCP + game.proto + Gateway 编排登录）。
2. Redis / MySQL 当前仍是**基础设施单点**（Registry/Session/Placement 可共用同一 Redis）。
3. GameLogic 故障恢复当前**不是**实时地图无损迁移；客户端需按新 Owner/epoch 重新进图。
4. **Unity 接入属于下一版本**；本阶段没有实现或验证 Unity。

## 标识

| 项 | 值 |
|----|-----|
| 候选版本 | `server-stable-v0.1.0-rc1` |
| 正式版本 | `server-stable-v0.1.0`（人工审核报告后） |
| Commit | 见 `run/release/<commit>/manifest.json` |
| C++ | 17 |
| brpc | **1.9.0**（`scripts/install_deps.sh`，SHA256 已钉死） |
| 报告目录 | `run/release/<commit>/` |

## 工具链（可复现）

```bash
# 系统包见 install_deps 打印；不依赖 runner 预装 /usr/local/include/brpc
./scripts/install_deps.sh --build-brpc   # → $HOME/.local/gamemesh-deps
./scripts/check_deps.sh --full           # 认可 PREFIX 或 /usr/local
./scripts/bootstrap_local_config.sh
./scripts/build.sh Debug
./scripts/build.sh Release
```

可选镜像：`Dockerfile.toolchain`（钉 brpc 1.9.0）；应用镜像见根 `Dockerfile`（build 阶段同样 `--build-brpc`）。

CI 分工：

- PR：`.github/workflows/ci.yml` → **lowlevel**（无 brpc）+ 语法/shellcheck
- push main：同文件 **full**（cache + `install_deps --build-brpc`）
- 夜间/手工：`.github/workflows/nightly-stable.yml` → `stable_gate.sh --full`

生成的 `*.pb.cc` 可能含工具尾随空白；门禁对生成文件排除 `git diff --check`，手写路径仍检查。

## 拓扑与支持范围

见 `AGENTS.md` 与 `docs/mmo-migration/topology-auth-session.md`：

- 客户端 ↔ VIP:8081 → Gateway×2；HTTP/Push 内网
- 登录：Auth → Session.AcquireSession → GameLogic.BindPlayer
- GameDB×2、GameLogic×2、Session×2、world(GlobalService)

## 部署 / 停止 / 回滚 / 故障演练

| 文档 | 用途 |
|------|------|
| `docs/runbook/deploy.md` | 部署 |
| `docs/runbook/rollback.md` | 回滚 |
| `docs/runbook/session-failure.md` | Session |
| `docs/runbook/logic-recovery.md` | Logic |
| `docs/runbook/gamedb-failure.md` | GameDB |
| `docs/runbook/gateway-failure.md` | Gateway |

演练脚本：`scripts/test_*_failover.sh`、`test_dynamic_logic_scale.sh`、`test_registry_outage.sh`。

## 硬门禁（同一最终 commit）

```bash
./scripts/check_deps.sh --full
./scripts/bootstrap_local_config.sh
./scripts/build.sh Debug && ./scripts/build.sh Release
./scripts/test.sh unit && ./scripts/test.sh integration
./scripts/test_sanitizers.sh asan && ./scripts/test_sanitizers.sh ubsan && ./scripts/test_sanitizers.sh tsan
START_CLUSTER=1 E2E_ROUNDS=20 ./scripts/test_e2e_20x.sh
LOAD_DURATION_SEC=1800 LOAD_CONCURRENCY=32 ./scripts/load_tcp_baseline.sh
SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh
./scripts/stable_gate.sh --full
```

成功时打印：`STABLE PASS — candidate server-stable-v0.1.0-rc1`
并生成 `run/release/<commit>/manifest.json`（含工具链、步骤 exit code、报告 SHA256、dirty）。

缩短时长的冒烟**不得**宣称正式稳定（门禁会 `STABLE BLOCKED`）。

## 稳定支持 vs Experimental

### 本版稳定支持

- Gateway×2 / Session×2 / GameLogic×2 / GameDB×2 / World×1 + Redis×1 + MySQL×1
- 登录链路、粘性 Dispatch、跨 GW 重连
- Session failover（sess-1 接管）、GameDB unknown-result 幂等查询
- GameLogic 崩溃语义：旧 epoch 失效 → 客户端重新登录/进图 → GameDB 持久化恢复
- 可靠 Push 跨 Gateway 重连不静默丢（缺口 → NEED_SNAPSHOT）

### Experimental（默认关闭，不进稳定硬门禁）

| 开关 | 能力 |
|------|------|
| `GAMEMESH_EXPERIMENTAL_DYNAMIC_SCALE=1` | 运行期 gl-2 动态扩容 / DRAINING 在线迁移演练 |
| `GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY=1` | Placement 自动无损接管调度 |
| `GAMEMESH_EXPERIMENTAL_REGISTRY_OUTAGE=1` | Registry outage 自愈演练 |

正式 `run_e2e_cluster.sh` / `stable_gate.sh --full` 默认上述为 `0`；开启后脚本才会跑对应测试。

## 已知限制

- Redis Registry / Session / Placement 同实例 → 生产应用 Sentinel/Cluster。
- MySQL 单点 → 生产主从/高可用另案。
- Logic 恢复非实时地图无损；旧 epoch 写入拒绝。
- Mail 同步 Claim 路径仍存在（非玩家串行链路）；玩家 BatchClaim 已异步。
- TSan 抑制仅限未 TSan 重编的第三方 `bthread/brpc/butil`（`tools/tsan_suppressions.txt`）。
- 低配机器（≤2 核 / ≤4GB）跑满并发 load 可能需下调 `LOAD_CONCURRENCY` 做冒烟，但正式候选仍须满足默认时长阈值；**不得自行降低成功率阈值后仍宣称原性能承诺**。

## Schema / Redis

- MySQL：`db/` EnsureTables / 幂等表。
- Redis 前缀：`gamemesh:dev:`（配置 `key_prefix`）。

未经人为授权不要创建 git tag / push。
