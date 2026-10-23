# 改动

相对 `86391f0` 的源码改动。没有提交，没有重启。

| 位置 | 行为 |
|---|---|
| `TcpReplySink` / 心跳 | 先丢掉可丢的旧移动帧再重试一次控制回包。成功计入 `reply_generated` 和 `reply_queue_accepted`。仍失败则 `reply_soft_drop` 或 `heartbeat_soft_drop`，日志带上 `send_buf` 和 `unsent`。不关连接，避免重连雪崩。已经提交的移动不会改成失败回包 |
| `SendBySession` | 回调改为返回 bool。不在连接的 EventLoop 上等待。其它线程最多等 200ms 的入队结果，不等 TCP ACK。入队失败返回 false，调用方记拒绝。回放里仍有该序号，客户端走 resync |
| `GameLogic` AOI 线程 | 取走批次时队列深度归零，同时记下 `aoi_worker_inflight` 和当时最老批次的年龄。发布结束后两者归零。没有加第二套异步，也没有按地图分片 |
| 快照 | 默认 `aoi_max` 从 128 改为 200，与 `MapRuntime` 可见上限一致。环境变量 `GAMEMESH_SNAPSHOT_AOI_MAX` 仍可覆盖 |
| `TcpConnection` | 暂存的最新移动帧最多挂一个 200ms 定时器。内核未发送降到 1KB 以内就写出；连接关闭后 weak_ptr 失效，不再武装。只剩这一帧时仍不注册 EPOLLOUT |

`reply_write_complete`、发布耗时的 P95/P99 没有做。现有数据是 `gamemesh_pushbatch_ms_sum`、`gamemesh_pushbatch_total`、`gamemesh_pushbatch_last_ms` 和 10 秒窗口最大。分位数标为未采集。

## 已运行

2026-10-23 13:22，Debug，`-j2`：

| 目标 | 结果 |
|---|---|
| `reactor_integration_test` | 通过。含内核忙时按住移动帧，以及排空内核后 200ms 定时器把这帧写出 |
| `world_snapshot_test` | 通过。不在本逻辑上的玩家仍是失败；129 个邻居的快照 `ok=true`，不是 `ERR_SNAPSHOT_TOO_LARGE` |
| `map_runtime_aoi_test` | 通过。日志里有一条预期的 `ERR_STALE_SEQ` |
| `gateway_conn_race_test` | 通过 |
| `auth_session_boundary_test` | 通过 |
| `gateway`、`gamelogic` | 链接成功。时间 13:21 / 13:22。正在跑的进程仍映射旧 inode |

## 未运行

605 人只心跳、多实例 5Hz、同实例热点、慢读端加高水位的整网压测，都未运行。全量 `test_unit.sh` / `test_all.sh` 这一轮没有再跑。
