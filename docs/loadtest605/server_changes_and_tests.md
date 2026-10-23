# 服务端改动与测试

`HEAD` 仍是 `b63fc6e`。改动未提交。13:44 的 gateway / gamelogic 进程还在跑旧 inode。磁盘上的 `build/test/gateway`、`build/test/gamelogic` 已在 14:48–14:49 链上新代码。没有重启，没有打正式压测。

客户端协议 `game.proto` 没改。`schema_sha256` 仍是 `edfd9fb92b1961dac7451d0b78be4e12ef9ee1424d9716d7e02dbcd5d237c6ae`。

## 改了什么

| 位置 | 行为 |
| --- | --- |
| `runtime/PushAdmit.cpp` | 先核对 session、玩家、网关、fence、generation、帧长，通过后才分配可靠序号。发送失败且序号已写入回放时记 `replay_pending`，不回滚序号。 |
| `runtime/brpc/GatewayPushServer.cpp` | 每条 `PushItemResult`（玩家、序号、是否入队、原因、是否待回放）。`queued=true` 只表示进了应用缓冲或移动合并槽。 |
| `game/GatewayConnRegistry` | 发送回调返回 `1` 入队、`0` 没进缓冲、`<0` 连接没了。 |
| `game/GameTcpGateway.cpp` | 跨线程等待仍是 200ms，等的是入队结果。超时后取消还没执行的回调，避免报失败的同时帧又写进缓冲。 |
| `runtime/brpc/GameLogicPush.cpp` | 推送前改用当前绑定的 gateway / session / fence / generation，不再沿用进图时的旧 session。按返回的逐条结果计数。日志不再写死 stale session/fence。 |
| `runtime/AoiRecipientGate` | `no_session` 或 `closed` 后暂停给该收件人组 AOI，`BindPlayer` 成功时清掉。 |
| `runtime/OpsMetrics.cpp` | `gamemesh_reliable_push_reject_total{reason=...}`。 |

没改：Move 速度、坐标校验、分线容量、AOI 可见上限、AOI 发布线程、快照上限 200、1KB 移动预算、200ms 停车重试、回复软丢重试。没有第二层异步 Push，也没有把 `QueueOneFunc` 成功说成 TCP 已写完。

## 指标

已有、继续用：`gateway_rx_frames`、`reply_generated`、`reply_queue_accepted`、`aoi_worker_inflight`、`pushbatch_ms_sum` / `pushbatch_total` / `pushbatch_last_ms` / `pushbatch_slow`（≥200ms）。

没有采集、这次也不补假数：

- 回包队列年龄的 p95 / p99
- 内核写完（`fully_written`）
- replay / FullSnapshot 成功次数

13:47–13:57 的回包排队高水位仍是约 19–20ms，那是窗口最大值，不是 p95。

## 测试

| 命令 | 结果 |
| --- | --- |
| `build/test/push_admit_test` | 通过。无会话、错 fence、旧 generation、跨网关、短帧、无发送回调都不分配序号。发送失败和连接关闭后序号仍是 11，并标 `replay_pending`。不可靠发送失败不分配序号。抑制在 Clear 之后恢复。 |
| `gateway_conn_race_test` | 通过。 |
| `map_runtime_aoi_test` | 通过。 |
| `aoi_subscription_invariant_test` | 通过。断线有一次 LEAVE，断线期间收不到 MOVE，重连后补 ENTER。 |
| `aoi200_subscription_test` | 通过。不重复 ENTER，LEAVE 后名额让给下一名，旧 session 不再当收件人。 |
| `gamelogic`、`gateway` | 编译并链接通过。 |

正式 605 人复测：**未跑**。分原因计数在线上还是 0，因为旧进程没换。

状态：判定顺序和抑制 **已修改未压测**。`push_admit_test` 覆盖的判定 **已修复且验证**。78/s 在旧日志上的原因 **已确认**，新二进制上的下降幅度 **无法确认**。
