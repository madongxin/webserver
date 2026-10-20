# 32 优化 backlog

## P0（本轮实测）

- **无**。未发现 OOM、鉴权绕过、双实体风暴。

## P1

| ID | 改动 | 范围 | 风险 | 回滚 | 状态 |
| --- | --- | --- | --- | --- | --- |
| P1-a | AOI MOVE 100ms 抽样 + `AoiEvent.server_time_ms` | `GameLogic.cpp` / proto | 客户端需新 schema | 回退合并策略 | **代码已合入 HEAD，待重启** |
| P1-b | TooFast 钳位接受 | `MapRuntime::Move` | 作弊者可沿方向爬速 | 恢复拒绝 | **代码已合入，待重启** |
| P1-c | 压测 scatter>0 A/B（配置） | manifest 试验环境 | 改变出生观感 | 改回 0 | 未做（需确认） |
| P1-d | 导出 lock_wait / candidate_count / move_ok | OpsMetrics | 低 | 删指标 | 建议下轮 |

## P2

- 机器人降速、处理 Hello 空转、ttl_sec、断线重连（客户端）。  
- 分线人数监控标注「占位 vs ONLINE」。  
- 修复/核查 `process_socket_recv_queue_bytes` 异常大值。  

## 不可做

- 关闭 LoginAdmission / 鉴权。  
- 破坏 fence / 有向订阅。  
- 正式环境重放攻击性压测（未确认前）。
