# 好友系统验收记录（2026-10-12）

## 版本

2026-10-12 11:39 复核：本地分支 `main`，HEAD 与 `origin/main` 都是 `784d0851679adc8355ae81a6fb1ccb56861d6356`（2026-10-12 11:31:31 +0800）。`git fetch --prune origin` 成功，领先和落后都是 0。默认分支是 `main`。工作区没有已跟踪文件的修改，没有执行 pull。未跟踪的 `.deps/`、提示词草稿和规格文档未改动。

`LoadIdempotency` 在 `4e5499d` 已把错误码复制到 `stored_code` 之后才 `mysql_free_result`。本次又读了该函数，释放后不再使用 `row`。同文件其他查询也是先复制列再释放。没有新的代码修改。

正在运行的集群是 2026-10-09 拉起的进程，不包含 `4e5499d` 和 `784d085`。这次没有重启。

## 功能矩阵

| 项 | 状态 | 证据 |
|---|---|---|
| `LoadIdempotency` 释放结果集后读 `row[1]` | 最新版本已经正确实现 | `4e5499d` 在 `mysql_free_result` 之前把错误码复制到 `stored_code`。本次源码第 210–215 行先复制 `stored_op/stored_code/stored_req/stored_peer/stored_name`，再释放 |
| 同文件其他 `mysql_fetch_row` | 最新版本已经正确实现 | `CountSql`、`LoadProfile`、`PendingId`、列表、`Accept`、`Reject` 都在释放前复制字段 |
| 同一钥匙重放成功和失败 | 最新版本已经正确实现 | `friend_store_test` 核对成功的 `ok/error_code/request_id/peer_player_id/idempotent_hit`，以及查无此人名字的失败重放 |
| 按名字串到另一个角色、id 与名字不一致、改名后旧名被占用 | 最新版本已经正确实现 | 同一测试覆盖。本次没有重写 |
| 跨服 | 需明确产品规则才能修改 | `SameRealm` 比较登录请求的 `server_id`，不是 `realm_id`，也不是 GameLogic 进程 id。角色表没有区服列。未改对外行为 |
| 等级、职业、头像、地图名 | 最新版本已经正确实现 | 没有这些列。`level` 为 0，不是伪造的 1。没有改备注协议 |
| 私聊依赖失败、黑名单整页、好友缓存只用完整一页 | 最新版本已经正确实现 | `GateWhisper`、`LoadBlockSet`、`HandleList`。本次没有停 Redis，也没有造多页数据 |
| 世界快照不含好友 | 确认存在且已写入契约 | `GameLogic::BuildFullStateSnapshot` 只填属性、背包和地图。契约要求重连后另拉三份好友列表 |
| 双账号全流程 | 未能验证 | 没有两个客户端登录联调 |

## 本次改动

没有改 C++、协议或数据库。只补充文档：

- `docs/friend-system-server-contract.md`：写明 `need_full_snapshot` 不是好友快照。
- `docs/friend_system_acceptance.md`：本记录。

## 测试

| 命令 | 结果 | 输出 |
|---|---|---|
| `./build/test/friend_store_test` | PASS | 2026-10-12 11:39 再跑，末尾 `OK friend_store_test`。并发输家有一次死锁和 `uk_actor_idempotency` 重复键日志，重试后仍只有一条申请 |
| `scripts/test_sanitizers.sh` | NOT RUN | 该脚本 `ENABLE_MYSQL=OFF`，编不出 `friend_store_test` |
| `./scripts/test_unit.sh`、`./scripts/test_all.sh` | NOT RUN | 本次没有跑 |
| 停 Redis 后的私聊、双客户端 | NOT RUN | 没有故障注入，也没有两个登录客户端 |

## Unity 合同与跨服

消息名和字段号不变。登录、换角、重连后按游标拉完好友列表、收到的申请、黑名单。`friend.presence.v1` 可以丢。`FriendBrief.level` 为 0 表示没有等级，不是 1 级。

跨服政策未定。按 id 且双方都在线、会话 `server_id` 都非 0 且不同时，搜索和申请按玩家不存在拒绝。按名字、离线、Redis 不可用都不走这条拒绝。

## 不能判定已全面验收

跨服规则未定。消毒器、停 Redis、双账号都没有在这次执行。线上进程也还不是 `4e5499d`。
