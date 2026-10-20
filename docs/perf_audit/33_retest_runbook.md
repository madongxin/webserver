# 33 隔离复测 Runbook

**环境**：与现网同规格隔离机；**禁止**默认打公网 EIP。`run/stress/login_report.json` 已示范 BLOCKED。

## 准备

1. 记录 `git rev-parse HEAD`、`build/test/gateway` mtime、`/proc/pid/exe`。  
2. `stop_formal.sh` && `GAMEMESH_ADVERTISE_HOST=10.0.0.2 start_formal.sh`（仅隔离约定主机）。  
3. 客户端 schema = `e6668114…`（含 AoiEvent.server_time_ms）。  
4. 压测前：`for p in 8080 8082 8090 8091 8093 8096; do curl -s localhost:$p/metrics > /tmp/m0_$p.txt; done`  
5. 机器人：405、40ms、20min、8081+8083；记录 bot_sha；打开 JSONL。  

## 用例

| ID | 只改一项 | 成功判据 |
| --- | --- | --- |
| R1 | 重启新二进制 vs 旧 | MOVE 推送间隔中位 ≤100ms；大跳减少 |
| R2 | scatter 0 vs 5 | aoi_move_events/s 下降，登录成功率不变 |
| R3 | bot move_speed 对齐 10 | TOO_FAST/UNWALKABLE 数量级下降 |
| R4 | 单线 200 人热点 | handle_peak、CPU、lock_wait（新指标） |

## 压测后

差分 metrics；跑 `python3 scripts/perf_audit/parse_logs.py --start … --end …`；填写 users/CCU/login_tps/move_rps/errors（缺则 NOT_OBSERVABLE）。

## 依赖

Redis/MySQL 本地；不要用正式账号库。cmake 构建 `-j 2`。
