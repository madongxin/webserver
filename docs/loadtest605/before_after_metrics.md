# 指标对照

不同构建的数字分开列。不能把一张表里的差值写成所有构建共同的结论。

## 构建 86391f0

11:35–11:43（UTC `2026-10-23T03:35:00Z`–`03:43:00Z`）是这一构建上的窗口。进程是 11:34 拉起的 `86391f0`。下面的差值只属于这一构建，来自 Prometheus，不是从图上估的。

| 指标 | 窗口差值 | 说明 |
|---|---|---|
| `gamemesh_reply_soft_drop_total` | 0 | 两台 Gateway 都没有增加 |
| `gamemesh_heartbeat_soft_drop_total` | 0 | 两台 Gateway 都没有增加 |

软丢不是这次 52% 移动超时的已证实主因。**待验证**。

这一构建之后换过二进制。那些窗口的计数不写在这张表里。没有对应 SHA 的新计数时，该项是缺失，不用 0 填充，也不要把超时下降写成已经解决。

再用 `scripts/loadtest_summary.py` 出一张表时，表头必须带那次进程的 `build_sha`。两个 SHA 的行不能相加，也不能合成一句「压测已经好了」。

## 现在能看的量

| 名字 | 含义 | 不是 |
|---|---|---|
| `gamemesh_reply_generated_total` | 心跳或 RPC 回包开始发送 | 客户端已匹配 |
| `gamemesh_reply_queue_accepted_total` | 回包进了应用缓冲，或当场 write 成功 | 内核写完。`reply_write_complete` 未采集 |
| `gamemesh_reply_soft_drop_total` | 丢掉旧移动帧后仍排不进控制回包 | 移动校验拒绝 |
| `gamemesh_heartbeat_soft_drop_total` | 心跳应答同样排不进去 | 人已下线 |
| `gamemesh_send_pending_max_bytes` | 10 秒窗口内应用缓冲加内核未发送的最大字节。当作队列高水位 | 历史累计 |
| `gamemesh_aoi_push_backlog` | 还排在 worker 前面的批次数 | worker 空闲 |
| `gamemesh_aoi_worker_inflight` | 已取走、`PushBatch` 还没结束的批次数 | 队列深度 |
| `gamemesh_aoi_batch_oldest_age_ms` | 最近一次取队列时最老批次的年龄 | 发布分位数 |
| `gamemesh_pushbatch_*` | 次数、毫秒总和、最近一次、≥200ms 的次数 | P95/P99。分位数未采集 |
| `gamemesh_aoi_move_shed_total` | 队列超过 64 批时丢掉的纯 MOVE | ENTER/LEAVE |

标签没有 `player_id`。P50/P95/P99 未采集。
