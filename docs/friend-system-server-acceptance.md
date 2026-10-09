# 好友双账号联调

客户端连 VIP `10.0.0.2:8081`。A、B 使用两个已登录账号。下面每一步都看服务端错误码，不看 message 文本。

1. A 搜索 B 的角色名或 PlayerID，应看到玩家且还不是好友。
2. A 申请 B，带新的 `operation_id`。B 收到 `friend.request.v1`，申请里有 A 的名字。同一 `operation_id` 再发一次，不产生第二条申请。
3. B 同意。两边好友列表都能看到对方。A 收到 `friend.added.v1`。同一 `operation_id` 再同意一次，不再推送。
4. A 删除 B。两边列表都没有对方。B 收到 `friend.removed.v1`。再用一把新钥匙删除，返回 `ERR_NOT_FRIEND`。
5. A 拉黑 B。B 再申请 A，A 侧不出现这条申请（`request_id` 为 0，接口成功）。B 给 A 发私聊，B 看到发送成功，A 收不到。
6. A 给 B 发私聊，返回 `ERR_ALREADY_BLOCKED`。
7. 停掉 Redis 后再让 B 给 A 发私聊，应返回 `ERR_DEPENDENCY_UNAVAILABLE`，并且 A 仍然收不到。恢复 Redis 后，私聊门禁恢复为上面的拉黑结果。
8. A 解除拉黑。B 重新申请，A 同意，两边列表恢复。
9. B 断线重连后拉取好友列表和申请列表，关系还在。在线状态以列表为准；断线宽限内不要当成已登出。

未在本环境自动跑通的项：真实双客户端、停 Redis 后的私聊、以及 ASan/TSan。不能据此写成已经完全上线。
