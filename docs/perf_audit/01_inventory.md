# 01 数据源盘点

## 仓库与运行版本

| 项 | 值 | 证据级别 |
| --- | --- | --- |
| `git rev-parse HEAD` | `3bb747ff2ed982f2d55298e0f9a2343085571ed9` | OBSERVED |
| 工作区 | 大量未提交改动（含本日 AOI/移动钳位，**未进正在跑的进程**） | OBSERVED `git status --short` |
| 正式集群启动 | 2026-10-15 **11:47:30–11:47:31** | OBSERVED `ps -o lstart` pid 22270…22448 |
| 压测时二进制 | `build/test/gateway` / `gamelogic` **(deleted)**，仍为 11:47 启动映像 | OBSERVED `readlink /proc/<pid>/exe` |
| 磁盘上新构建 | gateway/gamelogic mtime **14:39–14:42**（压测之后） | OBSERVED `ls -la build/test/` |
| `GAMEMESH_ADVERTISE_HOST` | 历史约定 `10.0.0.2`；本轮未改启动参数 | INFERRED from prior ops |

**结论**：13:39 压测跑在 **11:47 映像**上；14:35 起合入的 AOI 抽样/`TooFast` 钳位 **未生效于本次压测**。

## 日志文件（本机 `run/formal/logs/`，时区与日志戳均为本地墙钟，形如 `20261015 HH:MM:SS`）

| 文件 | 服务/实例 | 大小(扫时) | 时间范围 | 轮转 |
| --- | --- | --- | --- | --- |
| gw0.log | gateway gw-0 :8081 | ~3.1M | 11:47:32–15:20+ | 无 `.gz` |
| gw1.log | gateway gw-1 :8083 | ~3.4M | 同上 | 无 |
| logic0.log | gamelogic gl-0 :8201 | ~4.1M | 同上 | 无 |
| logic1.log | gamelogic gl-1 :8202 | ~3.6M | 同上 | 无 |
| session.log / session1.log | session×2 | ~1.5M each | 同上 | 无 |
| gamedb0/1.log | gamedb×2 | 3.5M / 1.0M | 同上 | 无 |
| world.log | world | ~1.1M | 同上 | 无 |

主机：单机 4 核 / ~3.6GB RAM。无 journald 项目专用单元、无 Docker 压测容器日志。

## 压测机器人产物

| 路径 | 说明 |
| --- | --- |
| `run/stress/login_report.json` | **另一场景**：`status=BLOCKED`，`reason=refusing public EIP`，users=2，**不是**本次 405 跑 |
| `scripts/load_game_login_stress.sh` | 仓库脚本；报告字段含 ramp/rps，**不能当真实 RPS**（提示词已强调） |
| Unity 联调压测面板 | 客户端侧；服务器无机器人 stdout。参数来自对话/截图：登录 405、间隔 40ms、在线 20 分、Gateway A=8081 B=8083 |

机器人进程日志：**本仓库不可见** → 机器人 CPU/FD/`Move sent/s` = **NOT_OBSERVABLE**（仅能从服务器收包与错误码反推）。

## 监控

| 源 | 状态 |
| --- | --- |
| Prometheus `/metrics`（各角色 HTTP） | OBSERVED 进程累计计数；**无**压测窗口时序抓取落盘 |
| Redis `INFO` / `SLOWLOG LEN=20` | OBSERVED 事后采样，非压测当时 |
| MySQL slow log | 变量可查；**无**压测窗口慢查询导出 → QPS/锁等待 **NOT_OBSERVABLE** |
| 主机 CPU/软中断/重传时序 | **NOT_OBSERVABLE**（无 sar/prometheus node 历史） |

## 同日两次登录洪峰（避免混跑）

| Run | 窗口 | Acquire/Login 量级 | 备注 |
| --- | --- | --- | --- |
| run_1222 | 12:22–12:28 | unique login≈406 | 早盘；中途大量提前断线 |
| **run_1339（本报告主窗）** | **13:39–14:03** | **unique login=409，enter=409** | 约 20 分钟在线后 14:01 集体 Logout |

解析输出：`docs/perf_audit/data/run_1339/`、`data/run_1222/`。
