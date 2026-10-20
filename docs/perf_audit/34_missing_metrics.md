# 34 全局指标缺口与埋点

| 问题 | 现有 | 缺口 | 建议埋点 |
| --- | --- | --- | --- |
| 锁是否先饱和 | LockInst 累加 wait_us | 未导出 / 无直方图 | `gamemesh_aoi_lock_wait_us_*` |
| 每次 Move 候选数 | candidate_sum | 无 count | 导出 `aoi_candidate_count` |
| Move 成败比 | error_code 累计 | 无 ok、无窗口 | `move_total{result}` |
| Push RPC 等待 | accept/reject | 无 latency | PushBatch client 耗时 |
| 登录阶段 | 日志事件 | 无 duration | Orchestrator stage_ms |
| 机器人发送 | — | 全缺 | bot JSONL |
| 主机资源 | — | 全缺 | pidstat/node_exporter |
| 指标可疑 | recv_queue_bytes≈5.6e8 | 实现存疑 | 审查聚合 |

只读日志 **不足以** 回答：mutex P99、万人登录、AOI=200 延迟曲线。
