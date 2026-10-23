# 指标

采集仍是各进程 metrics HTTP，Prometheus job `gamemesh`。没有新端口，8094 没有对公网放开。标签只用 `instance`、`kind`、`stage`、`code`。没有 `player_id`、`session_id`、`request_id`。

没有直方图。P50 / P95 / P99 未采集。尾部只有 `gamemesh_pkt_stage_max_ms` 的 10 秒窗口最大，以及 `ms_sum / count` 的均值。

## 和这次超时对应的已有指标

| 指标 | 含义 | 不要当成 |
|---|---|---|
| `gamemesh_pkt_stage_max_ms{kind,stage}` | 10 秒窗口最大毫秒。阶段见 `server_request_trace.md` | 客户端 RTT，或内核写完 |
| `gamemesh_pkt_stage_ms_sum` / `count` | 累计和与次数 | 分位数 |
| `gamemesh_move_hold_total` | 内核未发送超过 1KB，最新移动帧留在应用缓冲 | 丢包。HELP 文本已改成 1KB，与 `kLatestMoveKernelBudget` 一致 |
| `gamemesh_move_flush_total` | 合并后的移动帧交给 write | 客户端已匹配 |
| `gamemesh_move_drop_total` | 非移动应用缓冲超过 64KB，这次移动帧没进队列 | 连接被关闭 |
| `gamemesh_heartbeat_ok_total` | 心跳帧已交给发送 | 客户端已读 |
| `gamemesh_heartbeat_soft_drop_total` | 心跳发送上限仍不够 | 人已下线 |
| `gamemesh_reply_soft_drop_total` | 应答没写入缓冲 | 业务失败 |
| `gamemesh_move_overloaded_total` | 邮箱深度达到 8，在 `HandleMove` 之前回 `ERR_OVERLOADED` | 移动校验拒绝 |
| `gamemesh_push_rejected_total` | Gateway 拒绝的推送条数 | 客户端收到的缺口次数。拒绝发生在分配序号之前时，不再制造缺口 |
| `gamemesh_push_accepted_total` | Gateway 接受并写入连接缓冲 | 客户端已应用 |
| `gamemesh_pushbatch_last_ms` | 最近一次同步 `PushBatch` 毫秒 | `HandleMove` 耗时 |
| `gamemesh_aoi_move_shed_total` | 发布队列超过 64 批时丢掉的纯 MOVE | ENTER / LEAVE |
| `gamemesh_mailbox_pending` | 地图邮箱待执行 | 延迟 |
| `gamemesh_send_pending_max_bytes` | 10 秒窗口内应用缓冲加内核未发送的最大字节 | 含全部游戏 TCP。`process_socket_send_queue_bytes` 不含这些连接 |

`EAGAIN` 和半帧写没有单独计数。`reply_queue` 在发送回调开始时停表，内核未发送体现在 `move_hold` 和 `send_pending_max_bytes`。

09:44–09:56 窗口的具体峰值写在 `server_request_trace.md`。那一窗口跑的是 09:41 的旧进程。
