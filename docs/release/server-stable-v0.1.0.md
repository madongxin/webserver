# server-stable-v0.1.0（候选）

状态：以 `./scripts/stable_gate.sh --full` 结果为准；未满足全部硬门禁时不得宣称 STABLE。

## 标识

| 项 | 值 |
|----|-----|
| 候选版本 | `server-stable-v0.1.0-rc1` |
| 正式版本 | `server-stable-v0.1.0`（人工审核后） |
| Commit | 见门禁报告 / `git rev-parse HEAD` |
| C++ | 17 |
| brpc | 1.9.x（见 `scripts/install_deps.sh`） |

## 工具链

```bash
./scripts/check_deps.sh --full
./scripts/install_deps.sh              # 打印系统包；可选 --build-brpc
./scripts/bootstrap_local_config.sh
./scripts/build.sh Debug
./scripts/build.sh Release
```

## 已通过测试（填写门禁实际路径）

- Unit / Integration / final_e2e
- ASan / UBSan / TSan（`scripts/test_sanitizers.sh`）
- 20× E2E（`scripts/test_e2e_20x.sh`）
- 30min TCP load（`scripts/load_tcp_baseline.sh`）
- 2h soak 或同 commit `SOAK_REPORT`（`scripts/soak_test.sh`）
- Session/Logic/GameDB 故障业务脚本

报告目录建议：`run/load/`、`run/soak/`。

## Schema / Redis

- MySQL：`db/` 内 EnsureTables / 幂等表；迁移保持向后兼容。
- Redis key 前缀：`gamemesh:dev:`（配置 `key_prefix`）；生产换前缀勿混用。

## 已知限制

- Redis Registry / Session / Placement 同实例时为控制面单点 → 生产用 Sentinel/Cluster。
- Logic 故障恢复非实时地图无损；客户端需重新进图。
- Mail BatchClaim 仍可能同步等待。

## 升级 / 回滚

见 `docs/runbook/deploy.md` 与 `docs/runbook/rollback.md`。

未经人为授权不要创建 git tag / push。
