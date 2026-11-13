# LEAVE pending 泄漏：根因与修复（2026-11-13）

指标名是 `gamemesh_aoi_leave_write_pending`。下面按代码回答，再写放弃后重连可能多出实体的设计。设计等确认，本轮不改协议。

## 1. pending 和 pending_oldest_ms 的口径

`gamemesh_aoi_leave_write_pending` 是 `runtime/AoiEvidence.cpp` 里 `g_leave_fail_since` 的条数。键是 `(viewer, subject)`，值是**第一次**写失败的时间。`AoiNoteLeaveWriteFail` 只有 `at == 0` 才写入，补发再失败不会改这个时间。

写入点：`MapRuntime::NoteAoiWrite`，`op == 3 && !ok` 时调用 `AoiNoteLeaveWriteFail`。成功调用 `AoiNoteLeaveWriteOk` 删这一对。放弃调用 `AoiNoteLeaveGiveUp` 删这一对。没有上限。

`pending_oldest_ms` 是 `EvidenceNowMs() - 这张表里最小的时间戳`。生产上 `EvidenceNowMs` 和 `MapRuntime::NowMs` 都是 `steady_clock`。

这不是 `leave_again`，也不是 V2 `notified` 里 `pending=Leave`。`GAMEMESH_AOI_V2_VISIBLE_SET` 默认关。`LeavePendingCountForTest` 数的是 `notified`，和这条指标不是同一张表。`leave_again` 是补发队列，断线时会被丢掉，但丢掉时不删证据表。

`viewer=online/offline` 用 `AoiSetViewerOnlineFn`。逻辑服注册的是 `MapRuntime::ViewerConnected`（实体还在且 `connected==true`）。回调没注册时，拆分序列不输出，不是 0。

## 2. 观察者下线后，以他为接收者的 pending LEAVE 有没有被清

| 路径 | 位置 | 清 `leave_again` 里他当接收者的项 | 清 `g_leave_fail_since` |
| --- | --- | --- | --- |
| `Disconnect` | `MapRuntime.cpp` `Disconnect` → `RemovePlayerInterest(..., erase_entity=false)` | 清。`RemovePlayerInterest` 对每条 `leave_again` 做 `erase(player_id)` | 不清 |
| `RemovePlayerInterest` | 同上，约 `leave_again` 的 `erase(player_id)` | 清接收者，不调用 `AoiNoteLeaveGiveUp` | 不清 |
| `ForgetViewer` | `MapRuntime.cpp` `ForgetViewer` | 不清 `leave_again`。只清 `visible_targets`、`notified[viewer]`、`last_left_ms` | 不清 |
| `RetryPendingLeaves` | 观察者不在 `entities` 或 `connected==false` 时 `drop` | 下一轮从 `leave_again` 删掉 | 不调用 `AoiNoteLeaveGiveUp`，证据表留下 |
| `MarkDisconnected` | Session Redis 状态 | 不进 `MapRuntime` | 不清 |
| 宽限清扫 | `ExpireDisconnected` 到期后走 `Leave` / `RemovePlayerInterest` | 同 `RemovePlayerInterest` | 不清 |
| 回到视野改发 ENTER | `RetryPendingLeaves` 的 `d.correct` 分支 | 从 `leave_again` 删掉 | 不调用 GiveUp，证据表留下 |
| 版本更新丢弃 | `interest_version` 比这条新 | 从 `leave_again` 删掉 | 不清 |

`notified[viewer]` 在 `ForgetViewer` 里整表删掉。默认配置下指标不读这张表。

## 3. 根因与修复

根因：放弃判断只在观察者**仍 connected** 时执行，比较的是 `leave_again.first_ms`，而且这条记录一旦被删掉重建，`first_ms` 会换成新的 `now_ms`。证据表的时间不会被补发改掉，也几乎不会被删掉。上面第 2 节的表是修之前的状态。

`RetryPendingLeaves` 里，观察者缺失或 `connected==false` 时直接 `drop` 并 `continue`，排在 `now_ms - first >= AoiLeaveRetryMaxMs()`（默认 600000）之前。下线后的条目走不到放弃。`given_up` 因此保持 0，而 `pending_oldest_ms` 按证据表的首次失败时间继续涨。

补发本身不重置证据表时间（`AoiNoteLeaveWriteFail` 见 `at != 0` 就跳过）。`RememberLeave` 只在 `version==0`（新记录）时写 `first_ms`。同一条记录上的补发只改 `last_ms` 和 `tries`。记录被删掉再创建时，`leave_again` 的年龄从头算，证据表的年龄不重置。线上 `given_up=0` 且最老超过 12 分钟，对得上「下线分支提前返回，证据表没人删」。

修复后：凡是从 `leave_again` 删掉 `(viewer, subject)`，都经过 `MapRuntime::DropPendingLeave`，同时删掉 `g_leave_fail_since` 里的同一对。原因分成 `viewer_offline`、`back_in_window`、`version_superseded`、`acked`、`given_up`。观察者离线（含宽限期内 `connected==false`）时立即丢弃，补发前再查一次，不在线就不 `EmitTo`。放弃按证据表里第一次写失败的时间计算，补发不改这个时间；没有证据行时才用 `leave_again.first_ms`。放弃记 `given_up`，日志最多 1 条/秒。同一观察者上次 LEAVE 写失败，或测试里登记的未发送字节超过 16KB 时，补发按 1s、2s、4s 退避，上限 30 秒。ENTER 写成功且证据表里还有这一对时，按 `back_in_window` 删掉证据行；这一步不删补发队列。每轮 `AdvancePresented` 放开地图锁之后计算 `gamemesh_aoi_leave_pending_inconsistent`。

## 4. 重连后 `AoiRecipientGate::Suppress` 会不会清

会。`BindPlayer` 成功时清：

- `runtime/brpc/GameLogicServiceImpl.cpp`：幂等绑定分支 `AoiRecipientGate::Instance().Clear(player_id)`，然后 `MapRuntime::Reconnect`。
- 同文件新绑定成功分支同样 `Clear(player_id)`，然后 `Reconnect`。

`Disconnect`、`MarkDisconnected`、宽限清扫不调用 `Clear`。`Suppress` 发生在推送 `NoSession` / `Closed`（`GameLogicPush.cpp`）。下线后 Suppress 还在，下次 BindPlayer 成功才清。

## 设计（任务 d，等确认，本轮不实现）

放弃或因下线丢掉 pending LEAVE 之后，不新增协议消息。客户端上可能还留着这个实体。

重连走现有全量视野：`MapRuntime::Reconnect` 把 `connected` 设回 true，按当前兴趣发一次快照（已有 ENTER/快照路径）。约定是：

- 快照里有的实体，客户端留下或更新。
- 快照里没有、客户端还留着的实体，由客户端按「这次快照是全量兴趣」删掉。
- 服务端不再为已经丢掉的 LEAVE 补发。

若客户端把快照当成增量合并，就会多出实体。那是客户端按全量替换还是按增量合并的问题，等确认后再决定要不要改客户端。服务端本轮不新增「强制删除」消息。

## 修复（任务 A）

- 观察者 `Disconnect` / `RemovePlayerInterest`（含宽限清扫走到这里、`connected==false`）时，删掉 `g_leave_fail_since` 里以他为接收者的行，计入 `gamemesh_aoi_leave_write_dropped_total{reason="viewer_offline"}`。补发前再查一次；人不在线就丢，不 `EmitTo`。
- 放弃年龄用证据表的首次失败时间。没有证据行时用 `leave_again.first_ms`。补发只改 `last_ms`，不改这两个时间。放弃仍记 `gamemesh_aoi_leave_write_given_up_total`，日志最多 1 条/秒，带观察者、实体、年龄。
- 同一观察者上次 LEAVE 写失败，或 `AoiViewerUnsentBytes` > 16384 时，补发按 1s、2s、4s…退避，上限 30 秒，这期间不把 LEAVE 放进推送批次。逻辑服的推送回包没有 Send-Q，生产上这个字节数是无数据，退避靠写失败返回值。测试可注册 `AoiSetViewerUnsentFnForTest`。
- `pending_oldest_ms` 增加 `{viewer="online"}` / `{viewer="offline"}`。某一侧没有 pending 时不输出该序列。
- 从 `leave_again` 删掉 `(viewer, subject)` 都走 `DropPendingLeave`，同时删 `g_leave_fail_since` 里的同一对。计数是 `gamemesh_aoi_leave_pending_drop_total{reason=...}`：`viewer_offline`、`back_in_window`、`version_superseded`、`acked`、`given_up`。
- ENTER 写成功时，如果证据表里还有这一对，按 `back_in_window` 删掉。
- 每轮 `AdvancePresented` 在放开地图锁之后统计 `gamemesh_aoi_leave_pending_inconsistent`：证据表有、补发队列没有的对数。正常是 0。
