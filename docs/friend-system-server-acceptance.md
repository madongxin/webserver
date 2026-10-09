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

## 名字申请幂等

按名字申请先做精确名字解析，再比对幂等。同一玩家、同一把钥匙：

- 角色 A 的名字申请成功后，再用 A 的 id 申请，返回第一次的结果。
- 再按角色 B 的名字申请，返回 `ERR_INVALID_ARGUMENT`，B 没有新申请。
- 名字查无此人后，不能改拿这把钥匙去申请另一个 id。
- 另一个玩家可以使用同一串钥匙。

数据库迁移 `config/migrations/0004_friend_idem_subject_up.sql` 给 `friend_op_idempotency` 增加 `subject_name`，并把 `friend_request` 的幂等唯一索引改成 `(from_player_id, idempotency_key)`。先执行 `./scripts/migrate_db.sh`，再换新的 GameDB / World 二进制。回滚用 `./scripts/rollback_db.sh`（对应 `0004_friend_idem_subject_down.sql`）。若已经有两个玩家写入了同一串钥匙，回滚到全局唯一索引会失败。旧行 `subject_name` 为空且 `peer_player_id` 为 0 时，不能再按名字复用这把钥匙。

## 本次自动化结果

命令：`./build/test/friend_store_test`。输出末尾 `OK friend_store_test`。并发用例里输家会打出一次死锁和若干 `uk_actor_idempotency` 重复键日志，随后重试命中已提交的申请，待处理申请仍是 1 条。

| ID | 结果 | 说明 |
|---|---|---|
| S01 | 通过 | 同一命令覆盖按 id 搜索、精确名字、空目标 `ERR_INVALID_ARGUMENT`、不存在 `ERR_PLAYER_NOT_FOUND`、重名 `ERR_NAME_AMBIGUOUS` |
| S02 | 通过 | 同钥匙先名字 A 再名字 B 为 `ERR_INVALID_ARGUMENT`，B 无申请；未解析名字不能改绑到另一个 id |
| S03 | 通过 | 同目标重试、改操作、改目标、改申请号均冲突或命中旧结果；不同玩家可共用钥匙字符串；并发同钥匙只有一条申请 |
| S04 | 部分通过 | 同意、同意重试、过期 `ERR_REQUEST_EXPIRED`、过期后新钥匙可再申请。拒绝未单测 |
| S05 | 通过 | 删除两边都消失；再删 `ERR_NOT_FRIEND`；同一把钥匙重试删除返回已存错误 |
| S06 | 部分通过 | 拉黑后 `BLOCK_GATE` 双向结果正确。解除拉黑和 Redis 缓存失效未在本命令中执行 |
| S07 | 未执行 | 未停 Redis，未发私聊 |
| S08 | 未执行 | 未造多页黑名单 |
| S09 | 未执行 | 未跑在线推送 |
| S10 | 未执行 | 未改跨服行为，见契约里的区服说明 |
| S11 | 部分通过 | 搜索结果 level 为 1。职业、头像、最后在线时间没有在本命令里核对 |
| S12 | 未执行 | 未做登录、断线、切角色 |
| S13 | 部分通过 | 并发插入冲突后没有第二条申请，随后重试返回已提交结果。未做中途杀连接 |
| S14 | 未执行 | 未走 Gateway 伪造 `player_id` |

`./scripts/test_all.sh`、ASan/TSan、双客户端联调本次都没有跑。集群进程没有重启，线上仍是旧二进制，要等迁移完成并换进程后才生效。
