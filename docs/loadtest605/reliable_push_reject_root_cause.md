# 可靠 AOI 推送拒绝：13:47–13:57

工作区 `HEAD` 记录时是 `b63fc6ea438a983761e412bbcefb3ff4ef22c1f3`。当时线上进程仍是 13:44 拉起的那一版（gateway `31946`/`31949`，gamelogic `31877`/`31880`）。这次编译覆盖了磁盘上的二进制，进程仍映射已删除的 inode，没有重启。

## 不是回包软丢

这一窗里 Gateway 收到的 Move 仍是几十毫秒，回包排队最多约 19–20ms，Heartbeat 约 3ms。`reply_soft_drop`、`heartbeat_soft_drop`、`move_overloaded` 都是 0。客户端 8081 发送队列最老约 12,172ms。那部分不在这次拒绝里。

## gl-1 的 78/s 是断线后的 AOI，不是 fence

`gamemesh_push_rejected_total` 在 gl-1 上的峰值大约 78/s。日志把每一条都写成 `(stale session/fence)`，这句是固定文案，不能当成原因。

`run/formal/logs/logic1.log` 里，13:47–13:57 的 `PushBatch rejected` 几乎全在 13:55:04–13:55:06：

| 秒 | 批次数 | rejected | accepted |
| --- | ---: | ---: | ---: |
| 13:55:04 | 37 | 64 | 1345 |
| 13:55:05 | 37 | 120 | 943 |
| 13:55:06 | 117 | 1938 | 445 |

三秒合计 2122 次拒绝。同一段 `FlushBag ... reason=tcp_disconnect` 在 gl-1 上有 184 个不同玩家，gl-0 上有 125 个，gl-0 的拒绝是 573。平均每个刚断线的玩家还会被再推十几次。

原始日志（部署中的 `GameLogicPush.cpp`，没有分原因）：

```text
20261023 13:55:04.561923Z INFO PushBatch rejected=1 accepted=48 messages=49 (stale session/fence)
20261023 13:55:04.916510Z INFO FlushBag player_id=27596 reason=tcp_disconnect
20261023 13:55:06 这一秒 117 批，rejected=1938
```

对应代码路径：

1. TCP 关闭后 Gateway `Forget` 掉 session。`GatewayPushServer.cpp` 的 `FindBySession` 失败，在分配 `server_seq` 之前就 `rejected++`。
2. GameLogic 的 `UnbindPlayer(tcp_disconnect)` 还排在玩家串行队列里，地图实体仍是 `connected`。
3. AOI 线程继续按进图时抄下的 session 组批。`ResolveItem` 看到 session 非空就不再向当前绑定要新会话。
4. 所以这 2122 次是 `NO_SESSION`：收件人已经不在 Gateway 上。不是 fence 不符，不是序号分配失败，也不是发送缓冲软丢。可靠 ENTER/LEAVE 在这次拒绝里还没有写入回放，不会单靠这批拒绝造出序号洞。

邻居如果还连着，LEAVE 仍发给他们。被拒绝的是已经不在线上的收件人自己。

## 修复后怎么读

新计数 `gamemesh_reliable_push_reject_total{reason}` 按进程分开，不要把 Gateway 和 GameLogic 加在一起。10 秒增量用 Prometheus `increase(...[10s])`。`reason` 只有：

`no_session`、`player_mismatch`、`gateway_mismatch`、`fence_mismatch`、`generation_mismatch`、`seq_allocation_failed`、`bad_frame`、`send_failed`、`closed`、`replay_pending`。

没有 player_id 标签。抽样日志最多每秒 4 条，带 `player_id`、`seq`，不带 fence。

断线玩家第一次 `no_session` / `closed` 之后会进入收件人抑制，直到下一次 `BindPlayer`。抑制不删实体，也不等于 Logout。

状态：根因已从现有日志确认。分原因计数和抑制已改在源码里，正式服仍是旧进程，压测前后对比 **未跑**。
