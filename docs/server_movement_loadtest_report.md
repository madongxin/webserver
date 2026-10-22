# 移动压测报告

本轮**没有**在新二进制上跑 S0–S7。没有 600、3000、10000 人的通过结论。正式集群未重启，线上进程仍是改动之前的二进制。

## 本轮实际执行

| 命令 | 结果 |
| --- | --- |
| `GAMEMESH_JOBS=2 ./scripts/build.sh Debug gateway` | 退出码 0。随后又编了带停放计数的 `gateway` 与 `reactor_integration_test` |
| `./build/test/reactor_integration_test` | 退出码 0。内核忙时最新移动帧留在应用缓冲且不写进内核；回包留在该帧前面；内核空闲时帧会写到对端 |

未跑：`test_two_player_aoi.sh`、`aoi200_subscription_test`、1 真人 + 1 Bot、10/50/100/200/201/250/400、600×5 Hz、3000/10000、5% 断线注入。这些档位记 **NOT_RUN**。

10000 人相对 1002（8 线 × hard 400 = 3200）是 **CAPACITY_BLOCKED**，不能为测试抬高 hard。

## 时间窗

没有与本次二进制对齐的压测时间窗。计划值不能当成实测：

| 量 | 计划 | 本二进制实测 |
| --- | --- | --- |
| Bot 计划 Move/s | 600 × 90% × 5 Hz = 2700 | NOT_RUN |
| 客户端已发送 Move/s | — | NOT_RUN |
| Gateway 收到的帧/s | `rate(gamemesh_gateway_rx_frames_total)`，含心跳 | NOT_RUN |
| GameLogic 成功 Move/s | NOT_COLLECTED，没有受理 counter | NOT_RUN |
| AOI MOVE | `rate(gamemesh_aoi_move_events_total)` | NOT_RUN |
| 写进内核的移动帧 | `rate(gamemesh_move_flush_total)` | NOT_RUN |
| 停放 | `rate(gamemesh_move_hold_total)` | NOT_RUN |
| P95/P99 | NOT_COLLECTED | 不能填 |

## 先前现象（旧二进制，不是本报告的通过证据）

用户在 2026-10-21 约 09:02–09:09 的压测里看到机器人从一开始越走越慢，直到停下。当时线上是 08:58 的进程：权威坐标仍接受，AOI 仍在生成，但出站在「应用缓冲 + 内核未发送 > 32 KB」时不再放入最新位置。那次没有在本文件里回放原始计数，避免把旧进程的数写成新二进制的结果。

新逻辑要等用户明确说「重启集群」之后才在正式进程里生效。重启前不要用监控页判断这次修复。

## 瓶颈判断（未在新二进制上测量）

下一轮若再变慢，按这个顺序看，不要先改速度或容量：

1. 压测机自己的发送是否低于计划 2700 Move/s。
2. Gateway `rx_frames` 是否跟着掉。掉了就是客户端在等 MoveRsp，不是 Logic 算不动。
3. `move_hold` 持续升高而 `move_flush` 下降：内核未发送字节停在 8 KB 以上，最新位置在等回包先走。
4. `move_drop` 升高：非移动数据超过 64 KB，AOI 暂停入队。
5. `send_pending_max_bytes` 长期高于 8 KB：客户端没有把字节读走。
6. Logic `mailbox_pending` 和 `aoi_push_backlog`：排队在地图邮箱或 PushBatch。
7. 各实例 `process_cpu_seconds_total` 与 RSS 分开看，不要取 max 当总和。

登录洪峰和稳定移动要分开计时。先等 EnterMap 结束，再看 Move 窗口。
