# 600 人压测指标词典

采集入口仍是各进程已有的 metrics HTTP，Prometheus job `gamemesh`。没有新端口。标签只用 `instance_id`、`role`、`kind`、`stage`。不放 `player_id`、`session_id`、`request_id`，也不把 `map_instance_id` 做成标签。

没有直方图。表里写 P50/P95/P99 的地方，当前都是 **未采集**。已有的尾部数据是 10 秒窗口最大值，或累计和除以次数得到的均值。

## 已有

| 指标 | 类型 | 含义 | 不能当成 |
|---|---|---|---|
| `gamemesh_gateway_rx_frames_total` / `rx_bytes_total` | 累计 | Gateway 收到的游戏帧 | 其中不全是移动 |
| `gamemesh_gateway_tx_frames_total` / `tx_bytes_total` | 累计 | 已交给发送路径的帧 | 不是客户端已匹配 |
| `gamemesh_pkt_stage_max_ms{kind,stage}` | 10 秒窗口最大，毫秒 | `handler` 收包线程处理，`rpc` 业务函数本身，`reply_queue` 从入队到该连接 EventLoop 开始写 | 不含邮箱排队，不含客户端 RTT |
| `gamemesh_pkt_stage_ms_sum` / `gamemesh_pkt_stage_count` | 累计 | 同一阶段的总和与次数。均值 = sum/count | 不是分位数 |
| `gamemesh_pkt_slow_total{kind}` | 累计 | 该 kind 任一阶段 ≥ 1000ms 的次数 | 不区分阶段 |
| `gamemesh_send_pending_max_bytes` | 10 秒窗口最大 | 应用发送缓冲 + 内核未发送字节 | 不是历史累计 |
| `gamemesh_move_offer_total` | 累计 | 进入每连接移动合并槽 | 不是 MoveRsp |
| `gamemesh_move_flush_total` | 累计 | 合并后的移动帧交给 write | 内核未确认客户端收到 |
| `gamemesh_move_hold_total` | 累计 | 内核未发送 > 8KB，最新移动帧留在应用缓冲，不 write | 不是丢弃 |
| `gamemesh_move_drop_total` | 累计 | 非移动应用缓冲已 > 64KB，这次移动帧没进队列 | 连接未关闭 |
| `gamemesh_heartbeat_ok_total` | 累计 | 心跳帧已交给发送 | 不是客户端已读 |
| `gamemesh_heartbeat_soft_drop_total` | 累计 | 心跳想发但发送上限仍不够 | 人还在线上，没有回包 |
| `gamemesh_reply_soft_drop_total` | 累计 | `SendFrame` 失败，应答没写入缓冲 | 业务可能已经成功 |
| `gamemesh_mailbox_pending` | 瞬时 | 地图邮箱待执行任务 | 不是延迟 |
| `gamemesh_rpc_offload_pending` / `fast_pending` | 瞬时 | 卸载池普通队列 / 登录快队列深度。上限 256 / 64 | |
| `gamemesh_aoi_push_backlog` | 瞬时 | 等待发布的 AOI 批次数 | |
| `gamemesh_push_accepted_total` / `push_rejected_total` | 累计 | Gateway 接受或拒绝的推送条数。拒绝含代际/会话不匹配 | 不是客户端收到 |
| `gamemesh_queue_overload_total` | 累计 | 编排 `TryPost` 失败后的显式拒绝 | |
| `gamemesh_eventloop_todo_last` / `batch_ms_last` | 瞬时 | 事件循环积压和上一批回调耗时 | `loop_ms` 接近 1000 是空闲 poll，不是卡死 |

## 本次新增

这些计数在新编译的 `gateway` / `gamelogic` 里。21:30 正在跑的进程没有。

| 指标 | 类型 | 含义 |
|---|---|---|
| `gamemesh_aoi_move_shed_total` | 累计，事件数 | 发布队列超过 64 批时丢掉的纯 MOVE 事件。ENTER/LEAVE 不计在这里，因为它们不会被这条路径丢掉 |
| `gamemesh_safe_pos_deferred_total` | 累计 | 节流安全点已交给卸载池，没有占住地图邮箱 |
| `gamemesh_safe_pos_defer_fail_total` | 累计 | 卸载池满，安全点保持 dirty，等下一次移动再试 |
| `gamemesh_move_overloaded_total` | 累计 | 移动在进入 `HandleMove` 之前回了 `ERR_OVERLOADED` |
| `gamemesh_pushbatch_total` | 累计 | GameLogic 发出的同步 `PushBatch` 次数 |
| `gamemesh_pushbatch_ms_sum` | 累计，毫秒 | 上述调用耗时之和。均值 = sum/total |
| `gamemesh_pushbatch_slow_total` | 累计 | 单次 `PushBatch` ≥ 200ms |
| `gamemesh_pushbatch_last_ms` | 瞬时，毫秒 | 最近一次调用耗时。不是分位数 |

## 闭环怎么对

按一个时间窗的增量，而不是进程启动以来的总量：

`Gateway 收到的移动` 服务端没有单独的移动收包计数。客户端发送数减去两个 Gateway 的 `rx_frames` 增量，只能得到全部帧，含心跳。

能对上的是：

- 移动应答写进发送缓冲：`tx` 里混有 AOI 和心跳，不能单独当 `MoveRsp`。
- 明确没写上的应答：`reply_soft_drop` 增量。
- 明确在业务前拒绝的移动：`move_overloaded` 增量。
- 心跳写出：`heartbeat_ok`。心跳没写出：`heartbeat_soft_drop`。
- AOI 移动交给内核：`move_flush`。停在应用缓冲：`move_hold`。因为缓冲里已有别的业务而没进队：`move_drop`。

`send_enqueued` / `move_hold` 不是 `fully_written`。`PushBatch` 成功不是客户端已经收到。内核 write 返回也不是客户端 pending 已匹配。

迟到回包没有单独指标。要区分“没生成 / 生成了没路由 / 已写入 socket / 客户端 pending 已清”，需要客户端按 seq 记 raw receive。

## 查询示例

10 秒窗口最大，按实例分开。把时间换成压测区间。

```promql
max by (instance_id) (max_over_time(gamemesh_pkt_stage_max_ms{kind="move",stage="handler",role="gateway"}[1m]))
max by (instance_id) (max_over_time(gamemesh_pkt_stage_max_ms{kind="move",stage="rpc",role="gamelogic"}[1m]))
max by (instance_id) (max_over_time(gamemesh_pkt_stage_max_ms{kind="heartbeat",stage="handler",role="gateway"}[1m]))
max by (instance_id) (max_over_time(gamemesh_mailbox_pending{role="gamelogic"}[1m]))
max by (instance_id) (max_over_time(gamemesh_send_pending_max_bytes{role="gateway"}[1m]))
sum by (instance_id) (rate(gamemesh_move_hold_total{role="gateway"}[1m]))
sum by (instance_id) (rate(gamemesh_move_flush_total{role="gateway"}[1m]))
sum by (instance_id) (rate(gamemesh_gateway_rx_frames_total{role="gateway"}[1m]))
```

新指标上线后：

```promql
sum by (instance_id) (rate(gamemesh_move_overloaded_total[1m]))
sum by (instance_id) (rate(gamemesh_pushbatch_slow_total[1m]))
sum by (instance_id) (rate(gamemesh_aoi_move_shed_total[1m]))
sum by (instance_id) (rate(gamemesh_safe_pos_deferred_total[1m]))
max by (instance_id) (gamemesh_pushbatch_last_ms)
```

## 小规模结果

10～50 人的新一轮没有跑。`map_runtime_aoi_test` 覆盖了“纯 MOVE 批次可丢、ENTER/LEAVE 保留”。`reactor_integration_test` 覆盖了内核发送队列满时最新移动帧停住、心跳/控制帧插到它前面。这两项都是单机套接字测试，不是 50 人在线。
