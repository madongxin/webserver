# 04 数据缺口

| 缺口 | 影响 | 建议下轮 |
| --- | --- | --- |
| 无机器人 stdout / Move sent 计数 | 无法核对 offered move RPS、客户端超时放弃 | 机器人写 JSONL：ts,player,event,seq,rtt |
| 无 Prometheus 远程写/本地 scrape 历史 | 累计计数无法切开 12:22 vs 13:39 | 压测前 `curl /metrics` 快照 + 压测后差分 |
| `aoi_candidate_count` / `aoi_lock_wait_us` 未导出或未刮 | 无法算平均候选数、锁等待 P95 | 导出 ServerStats 已有原子量 |
| 无 Move 接受总数计数器 | 拒绝率分母缺失 | `move_ok_total` / `move_reject{code}` |
| 无阶段耗时（Auth/Acquire/Bind/Enter）起止配对字段 | 登录 P99 **NOT_OBSERVABLE** | 编排器打 `stage_begin/end` + duration_ms |
| MySQL slow log / 连接池等待时序 | 不能证明/否定 DB 根因 | 压测窗打开 slow_query；导出 pool wait 指标 |
| Redis latency 历史 | 仅事后 INFO | `LATENCY HISTORY` / 慢日志落盘 |
| 主机 CPU/软中断/网卡重传 | 不能谈「核跑满」 | node_exporter 或 `pidstat` 随跑 |
| `process_socket_recv_queue_bytes`≈5.6e8 且连接≈0 | 指标可疑，不能当背压证据 | 核查 SIOCINQ 聚合实现 |
| 运行中 `(deleted)` 二进制 | 与 HEAD 新代码不一致 | 压测前重启并记录 `build id` / mtime |

解析器：`scripts/perf_audit/parse_logs.py`；边界测试：`scripts/perf_audit/test_parse_logs.py`（OK）。
