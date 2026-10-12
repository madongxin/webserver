# 好友系统审查记录

审查起点：本地 `main` 与 `origin/main` 都是 `76e55debebc53e2c7e3dac3cc03355d1090e265e`（2026-10-09 13:27:30 +0800）。`git fetch --prune origin` 成功，领先/落后都是 0。工作区原有未跟踪文件未改动。

| 项 | 状态 | 证据 |
|---|---|---|
| 幂等命中在 `mysql_free_result` 之后读 `row[1]` | 已修复 | `FriendStore.cpp` `LoadIdempotency`。错误码在释放结果集之前复制到 `stored_code` |
| 按名字申请串到另一个角色 | 已实现 | `Apply` 先解析名字再 `TakeIdempotency`；`friend_store_test` 覆盖名字 A/B、id 与名字不一致、改名后旧名被占用 |
| 同一把钥匙重放成功和失败 | 已修复测试 | 成功重放核对 `ok/error_code/request_id/peer_player_id/idempotent_hit`；查无此人的名字重放核对失败结果 |
| 跨服 | 阻塞 | `FriendService::SameRealm` 比较的是登录请求里的 `server_id`，不是 `realm_id`。角色表没有区服列。未改对外行为 |
| 等级、职业、头像 | 已实现 | `player_profile` 没有这些列。`level` 保持 0，不填 1。没有改备注协议 |
| 私聊门禁失败拒绝、黑名单必须整页、好友缓存只用完整一页 | 已实现 | `GateWhisper`、`LoadBlockSet`、`HandleList`。本次没有重写，也没有停 Redis 复测 |
| 操作者身份 | 已实现 | `GameService` 调用 `ApplyTrustedPlayerId`。本次没有伪造客户端 `player_id` 的联调 |
| 离线恢复 | 已实现 | `friend.request.v1` / `friend.added.v1` / `friend.removed.v1` 进当前会话重放；`friend.presence.v1` 可丢。长期离线以列表为准。本次没有双客户端重连 |

`FriendStore.cpp` 里其余 `mysql_fetch_row` 都在 `mysql_free_result` 之前把要用的列复制出去。`CountSql`、`LoadProfile`、`PendingId`、`Accept`、`Reject` 属于这一类。

ASan/UBSan 脚本 `scripts/test_sanitizers.sh` 使用 `ENABLE_MYSQL=OFF`，编不出 `friend_store_test`，所以这次没有用消毒器跑幂等重放。
