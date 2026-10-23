# 客户端对照

服务端这次不改 Unity。下面是再跑一档时需要客户端记下的原始帧，用来区分“服务器没回”和“回了但对不上序号”。

## 每个超时的移动或心跳记一条

- 连接还在不在，Gateway 端口是 8081 还是 8083。
- 请求类型、`seq`、发出的墙钟。
- 是否在截止时间前收到同一 `seq` 的帧。收到时记 `ok`、`error_code`、`MoveRsp` 里的坐标。
- 截止后若又收到同一 `seq`，记 `late_rsp`，不要丢弃这条日志。这不等于服务器没发。
- 收到 `ERR_OVERLOADED` 时单独计数。这是邮箱忙，不是速度或坐标校验失败，也不要算进“拒绝移动”的校验栏。

## 不要改的行为

2 秒丢掉 pending、心跳 12 秒算超时，这两条先保持。服务端要在这个时限内回成功或明确错误。把超时改成 30 秒不能当作这次修复的结果。

## 服务端能对上的数

- 心跳写出：`gamemesh_heartbeat_ok_total`。没写出：`gamemesh_heartbeat_soft_drop_total`。
- 应答没进发送缓冲：`gamemesh_reply_soft_drop_total`。
- 移动在业务前被拒绝：`gamemesh_move_overloaded_total`。这个计数在新二进制里，21:30 的进程还没有。
- 内核未发送超过 8KB 时停住的 AOI 移动：`gamemesh_move_hold_total`。交给 write 的是 `gamemesh_move_flush_total`。

`tx_frames` 和 `PushBatch` 成功都不能当成客户端 pending 已匹配。

## 建议的一小档

先 10 人，只心跳和分线，不移动。再 10 人移动。确认没有非预期超时后，再考虑 100 人。600 人仍要单独批准，并且新二进制要先换上。换进程会清空在线状态，客户端需要重新登录。
