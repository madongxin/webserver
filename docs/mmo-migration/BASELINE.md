# 基线采集说明与记录模板

## 目的

在改造前冻结一组可对比数字，后续阶段回归时与同配置对比。

## 环境记录（每次必填）

| 项 | 值 |
|----|-----|
| 日期 | |
| 主机 | |
| 提交 | `git rev-parse --short HEAD` |
| 构建类型 | Debug / Release |
| CMake options | MYSQL/REDIS/BRPC/… |
| 启动命令 | `./build/test/server 8080 8081` |
| 依赖 | MySQL / Redis 是否可用 |

## 采集步骤

1. 启动 `server`（配置好 `config/mysql.cnf` / `redis.cnf` 如需要）。
2. 空载稳定 30s 后：

```bash
./scripts/capture_baseline.sh http://127.0.0.1:8080
```

3. 若有负载（手动客户端 / 后续压测器），负载中再采一次，文件名带后缀。

## 现有可记录指标

从 `/metrics` 抄录或由脚本落盘：

- `process_cpu_seconds_total`
- `process_resident_memory_bytes`
- `process_open_fds` / `process_threads`
- `gamemesh_eventloop_tick_seconds` / `_peak_seconds`
- `gamemesh_logic_handle_seconds` / `_peak_seconds`

## 缺口（本阶段不强制有数）

| 指标 | 状态 |
|------|------|
| 活跃连接数 | 未导出 |
| 消息 QPS | 无 |
| 延迟 p50/p95/p99 | 无直方图 |
| 错误率 | 无 |
| tcp_send/recv_queue_bytes | snapshot 有，/metrics 无 |

## 轻量功能回归（阶段 0）

```bash
./build/test/mail_unit_test
./build/test/file_hash_test
# 可选：mail_claim_integration_test（需 MySQL）
# 可选：ENABLE_BRPC 时 mail_brpc_client 手工跑 Login
```

协议行为应与改造前一致（本阶段未改业务代码）。

## 样本目录

脚本默认写入：

`docs/mmo-migration/baselines/YYYYMMDD-HHMMSS-metrics.txt`
