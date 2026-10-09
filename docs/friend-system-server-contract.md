# 好友系统联调契约

协议消息名和字段号不变。客户端继续用现有 `Friend*` 请求和 `friend.request.v1` / `friend.added.v1` / `friend.removed.v1` / `friend.presence.v1`。

## 身份

Gateway 用登录会话覆盖 `player_id`。服务端不信任客户端自报的操作者。同意和拒绝只允许申请接收方。

## 幂等

写操作必须带 `operation_id`，最长 96 字节，空串会被拒绝。同一个玩家、同一把钥匙、同一种操作、同一个目标，重试返回第一次的结果，不再发通知。同一把钥匙改去删除、改申请号，或改成另一个角色，返回 `ERR_INVALID_ARGUMENT`，不执行新操作。

按名字申请时，先用 `player_profile.player_name` 的精确匹配解析出玩家，再比对幂等。解析成功后，钥匙绑定这个玩家 id。同一把钥匙再按这个 id 申请，或先按 id 再按该角色当前名字申请，都返回第一次的结果。同一把钥匙先按角色 A 的名字、再按角色 B 的名字，返回 `ERR_INVALID_ARGUMENT`，B 没有新申请。同一次请求里如果同时带了 id 和名字，两者必须指向同一个角色，否则同样返回 `ERR_INVALID_ARGUMENT`，不创建申请。名字查无此人时，钥匙绑定这次提交的原文字符串；不能改拿去申请另一个 id 或另一个名字。角色改名后，这把钥匙不会套到后来占用旧名字的角色上。名字按客户端提交的字节比较，服务端不另做大小写折叠。不同玩家可以各自使用同一串 `operation_id`。

升级前已经写入、且 `subject_name` 为空、`peer_player_id` 为 0 的旧幂等行，无法分辨当初的名字，之后不能再用这把钥匙按名字申请。

## 私聊

- 自己拉黑对方：`ERR_ALREADY_BLOCKED`，消息不发出。
- 对方拉黑自己：接口仍返回成功，消息不投递给对方。响应里不出现“被拉黑”。
- 黑名单查不出来（Redis、GameDB 或分页不完整）：`ERR_DEPENDENCY_UNAVAILABLE`，`retryable=true`，消息不发出。这不是拉黑。

## 在线

`MarkDisconnected` 不是登出。断线宽限内仍算在线。在线状态推送可丢，不进重放。申请、结成好友、删除进当前会话的重放；完全离线后以好友列表和申请列表为准。推送失败不回滚已经提交的关系。

## 资料

`player_profile` 和 `PlayerAttributes` 只有名字和战斗属性，没有等级、职业、头像、地图名。好友资料里的 `level`、`profession` 保持 0，`avatar` 和 `map_name` 留空。0 不是 1 级。`remark` 来自 `friend_relation.remark`，没有单独的改备注协议。`online` 和 `last_online_time` 来自会话：在线或断线宽限都算在线，时间是 unix 秒，离线且没有记录时为 0。

## 区服

角色表没有持久化的区服字段。会话里的 `server_id` 来自登录请求 `LoginReq.server_id`，不是 `realm_id`，也不是 GameLogic 进程 id。没有确认过的跨服规则，所以现有行为保持不变：按 id 搜索或申请时，只有双方都在线且这个 `server_id` 都非 0 且不一致，才按玩家不存在拒绝。任一方离线、Redis 不可用、或按名字申请，都不在这里拒绝。当前集群是同一个 GameDB。

## 消息

字段号不变。`player_id` 一律由 Gateway 覆盖。`operation_id` 是客户端生成的幂等键，`request_id` 是服务端申请号，两者不是一回事。

| 消息 | 主要字段（数字是 protobuf tag） |
|---|---|
| `FriendSearchReq` | `player_id=1`，`target_player_id=2`，`exact_name=3` |
| `FriendSearchRsp` | `ok=1`，`error_code=3`，`player=4`，`relation=5` |
| `FriendApplyReq` | `player_id=1`，`target_player_id=2`，`exact_name=3`，`operation_id=4` |
| `FriendApplyRsp` | `ok=1`，`error_code=3`，`request_id=4`。对方拉黑自己时成功且 `request_id=0` |
| `FriendRequestListReq` | `player_id=1`，`cursor=2`，`page_size=3`。只返回收到的待处理申请 |
| `FriendRequestListRsp` | `requests=4`，`next_cursor=5`。游标是上一页最后的 `request_id`，降序。默认 20，最大 50 |
| `FriendAcceptReq` / `FriendRejectReq` | `player_id=1`，`request_id=2`，`operation_id=3` |
| `FriendAcceptRsp` | `peer=4` 是对方资料 |
| `FriendDeleteReq` | `player_id=1`，`friend_player_id=2`，`operation_id=3` |
| `FriendBlockReq` / `FriendUnblockReq` | `player_id=1`，`target_player_id=2`，`operation_id=3` |
| `FriendBlockListReq` | `player_id=1`，`cursor=2`，`page_size=3`。游标是 `blocked_player_id`，升序。默认和上限都是黑名单容量 |
| `FriendListReq` | `player_id=1`，`cursor=2`，`page_size=3`。游标是 `friend_player_id`，升序。`next_cursor` 为空表示最后一页。默认和上限都是好友容量 |
| `FriendBrief` | `player_id=1`，`name=2`，`level=3`，`profession=4`，`avatar=5`，`online=6`，`last_online_time=7`，`map_name=8`，`remark=9` |
| `FriendRequestPush` | 推送类型 `friend.request.v1`。`request_id=1`，`applicant=2`，`created_at=3`，`expire_at=4`。可靠 |
| `FriendAddedPush` | `friend.added.v1`，`peer=1`。可靠 |
| `FriendRemovedPush` | `friend.removed.v1`，`friend_player_id=1`。可靠 |
| `FriendPresencePush` | `friend.presence.v1`，`friend_player_id=1`，`online=2`，`last_online_time=3`。可丢，不进重放 |

关系枚举：0 无，1 好友，2 已发出申请，3 收到申请，4 自己拉黑了对方。没有“被对方拉黑”。

可重试：`ERR_OPERATION_TOO_FREQUENT`、`ERR_RELATION_CONFLICT`、`ERR_DEPENDENCY_UNAVAILABLE`。不要换结果重试：`ERR_INVALID_ARGUMENT`、`ERR_ALREADY_FRIEND`、`ERR_NOT_FRIEND`、`ERR_REQUEST_EXPIRED`、`ERR_FRIEND_LIMIT`、`ERR_TARGET_FRIEND_LIMIT`、`ERR_PENDING_LIMIT`、`ERR_WRONG_ROUTE`。申请默认 7 天后过期。
