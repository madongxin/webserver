# 稳定最小闭环 — 阶段三进度与结论

对照：`docs/GameMesh_Cursor_Server_Stable_Minimum_Closure_e8ab08f.md`  
工作树 commit（dirty）：`e8ab08fc1b2beef59d106fd1ac75bb9e43860ba5` + 阶段一/二/三未提交改动。

## 工程交付（已落地）

| 项 | 状态 |
|----|------|
| brpc 1.9.0 SHA256 钉死 + `install_deps --build-brpc` | **已做** |
| `check_deps`/`build.sh` 认可 `GAMEMESH_DEPS_PREFIX` | **已做** |
| `Dockerfile.toolchain`；根 Dockerfile 校验构建 brpc | **已做** |
| CI：PR=lowlevel；push main=full+brpc cache；nightly=`stable_gate --full` | **已做** |
| git diff --check / bash -n / shellcheck；生成 pb 排除 | **已做** |
| load/soak JSON（分位、RSS/FD/线程、错误计数） | **已做** |
| `run/release/<commit>/manifest.json` | **已做** |
| `docs/release/server-stable-v0.1.0.md` + deploy runbook | **已做** |
| placement 测试隔离前缀；mutation 并发≤池大小；Logic 杀后摘除 registry | **已做** |

## 同 commit 真实门禁结果（本机）

主机：2 核 / 3.5GiB（见 manifest toolchain）。

| 步骤 | 结果 |
|------|------|
| check_deps / bootstrap / shellcheck / bash -n | PASS |
| Debug + Release 构建 | PASS |
| unit / integration | PASS |
| final_e2e + 五类故障脚本 | PASS |
| sanitizers (asan/ubsan/tsan) | PASS |
| E2E 20× | PASS |
| load 1800s concurrency=8 | **FAIL** success_rate **94%** < 95%（timeout_like≈296；无进程退出） |
| soak 7200s | **未执行**（load 失败中断） |

结论输出：

```text
STABLE BLOCKED
```

报告：`run/release/e8ab08fc1b2beef59d106fd1ac75bb9e43860ba5/manifest.json`  
门禁日志：`/tmp/p3_stable_full.log`；load 摘要见该日志末尾。

## 是否达到 server-stable-v0.1.0-rc1

**否。** 剩余阻塞：

1. 30 分钟负载成功率需 ≥95%（本机 8 并发为 94%；建议 `LOAD_CONCURRENCY=4` 重跑或换更强机器跑默认 32）。
2. 通过 load 后仍需完整跑通 `SOAK_DURATION_SEC=7200`。
3. 建议在**干净提交**上再跑一遍 `./scripts/stable_gate.sh --full`（当前 dirty=true）。

重跑建议：

```bash
# 低配机
LOAD_CONCURRENCY=4 LOAD_DURATION_SEC=1800 ./scripts/load_tcp_baseline.sh
SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh
./scripts/stable_gate.sh --full   # 正式候选仍须整链
```

## 已知限制（写入发布说明）

- Redis/MySQL 基础设施单点。
- Logic 恢复非实时地图无损。
- Unity 未接入。
- 本机内存紧时 sanitizer/load 需控制并发；正式发布机应 ≥4 核 / 8GiB。

**未自行 tag / push。**
