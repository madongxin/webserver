# 移动 / AOI 指标字典

观察页是 GameDB HTTP `http://127.0.0.1:8094/monitor`，数据来自 Prometheus `job=gamemesh`。每个进程自己的 `/metrics` 才是计数源，不要把多实例 `max` 当成总量。

| 进程 | HTTP |
| --- | --- |
| gateway-0 / 1 | 8080 / 8082 |
| gamelogic-0 / 1 | 8090 / 8091 |
| session-0 / 1 | 8093 / 8096 |
| gamedb-0 / 1 | 8094 / 8095 |

标签只有有限的 `code`、线程 `state`、包种。没有 `player_id`、`session_id`、`bot_index`。`map_instance_id` 也不做生产标签。热路径是原子加，不打每步日志。

速率用 `rate(counter[1m])` 或相邻两次 scrape 的差除以秒。下面的 counter 都是进程启动后的累计值。

## Gateway

| 名称 | 类型 | 单位 | 含义 |
| --- | --- | --- | --- |
| `gamemesh_gateway_tcp_connections` | gauge | 个 | 当前游戏 TCP，含未登录 |
| `gamemesh_gateway_rx_frames_total` | counter | 帧 | 客户端进来的全部帧，含心跳，不是 Move/s |
| `gamemesh_gateway_rx_bytes_total` | counter | 字节 | 入站字节 |
| `gamemesh_gateway_tx_frames_total` | counter | 帧 | 出站帧，含回包和 AOI |
| `gamemesh_gateway_tx_bytes_total` | counter | 字节 | 出站字节 |
| `gamemesh_move_offer_total` | counter | 帧 | 纯移动 AOI 进入该连接的合并槽 |
| `gamemesh_move_coalesce_total` | counter | 帧 | 合并进已经排好的 flush，没有再排队 |
| `gamemesh_move_flush_total` | counter | 帧 | 最新移动帧交给了内核写路径 |
| `gamemesh_move_hold_total` | counter | 帧 | 内核未发送字节超过 8 KB，帧只留在应用缓冲，没有 write |
| `gamemesh_move_drop_total` | counter | 帧 | 非移动数据已经超过 64 KB，这一帧没入队。位置留在合并槽，200 ms 后再试 |
| `gamemesh_move_pending_entities` | gauge | 实体 | 合并槽里还没发出的实体数，是瞬时值 |
| `gamemesh_send_pending_max_bytes` | gauge | 字节 | 最近 10 秒窗口内，应用缓冲 + 内核未发送字节的最大值。窗口结束会换 0，不是累计 |
| `gamemesh_heartbeat_soft_drop_total` | counter | 次 | 心跳回包没写进缓冲，连接保持 |
| `gamemesh_reply_soft_drop_total` | counter | 次 | 其它回包软丢，不关连接 |
| `gamemesh_online_players` | gauge | 人 | 本 Gateway 已绑定的不同玩家 |
| `gamemesh_pkt_stage_max_ms` | gauge | 毫秒 | 最近 10 秒某种包、某一阶段的最大值。标签是阶段和包种 |

`process_socket_send_queue_bytes` 是 `SIOCOUTQ`（含已在路上的未确认字节），不是应用缓冲，也不能代替 `gamemesh_send_pending_max_bytes`。

P50/P95/P99：**NOT_COLLECTED**。只有窗口最大值和 `gamemesh_pkt_stage_ms_sum` / `gamemesh_pkt_stage_count` 的均值。不能用均值填 P95。

查询例子：

```promql
rate(gamemesh_move_flush_total{job="gamemesh"}[1m])
rate(gamemesh_move_hold_total{job="gamemesh"}[1m])
rate(gamemesh_move_drop_total{job="gamemesh"}[1m])
gamemesh_send_pending_max_bytes
```

`flush` 上升表示最新位置正在写进内核。`hold` 上升表示内核里还有旧字节，新位置在等它排空。`drop` 上升表示非移动数据已经堵住，观察者会暂时看不到更新。

## GameLogic / Move

| 名称 | 类型 | 单位 | 含义 |
| --- | --- | --- | --- |
| `gamemesh_aoi_move_events_total` | counter | 事件 | 已经生成、准备推给某个观察者的 MOVE。一个人移动会被每个观察者计一次 |
| `gamemesh_aoi_sub_enter_total` | counter | 事件 | 订阅 ENTER |
| `gamemesh_aoi_sub_leave_total` | counter | 事件 | 订阅 LEAVE |
| `gamemesh_aoi_candidate_sum` | counter | 人 | `VisibleIds` 扫到的候选人数之和 |
| `gamemesh_aoi_visible_query_us_sum` | counter | 微秒 | 这些查询耗时之和 |
| `gamemesh_move_err_stale_seq_total` | counter | 次 | `ERR_STALE_SEQ` |
| `gamemesh_move_speed_clamp_total` | counter | 次 | 超速后被缩短 |
| `gamemesh_move_walk_slide_total` | counter | 次 | 缩短后收回到可走格 |
| `gamemesh_error_code_total{code}` | counter | 次 | 公网错误码。标签是有限错误码集合 |
| `gamemesh_mailbox_pending` | gauge | 个 | 玩家串行队列待处理 |
| `gamemesh_aoi_push_backlog` | gauge | 个 | 等待 PushBatch 的 AOI 入队深度 |
| `gamemesh_logic_handle_seconds` | gauge | 秒 | 最近一次 GameLogic 处理耗时，不是直方图 |

Move 成功次数没有单独的 counter。用 Logic 日志里的拒绝，加上 `aoi_move_events` 只能说明 AOI 生成了，不能代替「MoveRsp 成功次数」。**Move 受理次数：NOT_COLLECTED**（没有按 accepted/rejected 拆开的 counter）。拒绝原因里，过期序号、超速缩短、可走收束有专有计数，其余走 `gamemesh_error_code_total`。

`pkt` 阶段 `move` / `rpc` 是 `HandleMove` 在地图邮箱上的耗时，不含客户端读完回包的时间。

## Session / 进程

| 名称 | 类型 | 单位 | 含义 |
| --- | --- | --- | --- |
| `gamemesh_session_online_players` | gauge | 人 | 该 Session 进程看到的 Redis ONLINE 集合大小 |
| `gamemesh_line_occupancy_total` | gauge | 人 | LINE 的 `map:occ` 人数和 |
| `gamemesh_ghost_occupancy` | gauge | 人 | `max(0, 占位 - ONLINE)`，宽限期内的幽灵占位 |
| `process_cpu_seconds_total` | counter | 秒 | 本进程用户态+内核态 CPU |
| `process_resident_memory_bytes` | gauge | 字节 | RSS |
| `process_open_fds` | gauge | 个 | 打开的 FD |
| `process_threads` | gauge | 个 | 线程数 |

Redis 延迟、MySQL 耗时、Reconnect 耗时直方图：**NOT_COLLECTED**。Reconnect 成功/失败仍走既有 Gateway 计数和日志，不在本字典里伪造分位数。

按实例看 CPU：

```promql
rate(process_cpu_seconds_total{job="gamemesh"}[1m])
process_resident_memory_bytes
gamemesh_session_online_players
gamemesh_gateway_tcp_connections
```
